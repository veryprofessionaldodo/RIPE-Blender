/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup bke
 *
 * High level `.blend` file read/write,
 * and functions for writing *partial* files (only selected data-blocks).
 */

#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_workspace_types.h"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_system.h"
#include "BLI_path_util.h"
#include "BLI_utildefines.h"

#include "IMB_colormanagement.h"

#include "BKE_addon.h"
#include "BKE_appdir.h"
#include "BKE_blender.h"
#include "BKE_blender_version.h"
#include "BKE_blendfile.h"
#include "BKE_bpath.h"
#include "BKE_colorband.h"
#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_ipo.h"
#include "BKE_keyconfig.h"
#include "BKE_layer.h"
#include "BKE_library.h"
#include "BKE_main.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_screen.h"
#include "BKE_studiolight.h"
#include "BKE_workspace.h"

#include "BLO_readfile.h"
#include "BLO_writefile.h"

#include "RNA_access.h"

#include "RE_pipeline.h"

#ifdef WITH_PYTHON
#  include "BPY_extern.h"
#endif

/* -------------------------------------------------------------------- */
/** \name High Level `.blend` file read/write.
 * \{ */

static bool clean_paths_visit_cb(void *UNUSED(userdata), char *path_dst, const char *path_src)
{
  strcpy(path_dst, path_src);
  BLI_path_native_slash(path_dst);
  return !STREQ(path_dst, path_src);
}

/* make sure path names are correct for OS */
static void clean_paths(Main *main)
{
  Scene *scene;

  BKE_bpath_traverse_main(main, clean_paths_visit_cb, BKE_BPATH_TRAVERSE_SKIP_MULTIFILE, NULL);

  for (scene = main->scenes.first; scene; scene = scene->id.next) {
    BLI_path_native_slash(scene->r.pic);
  }
}

static bool wm_scene_is_visible(wmWindowManager *wm, Scene *scene)
{
  wmWindow *win;
  for (win = wm->windows.first; win; win = win->next) {
    if (win->scene == scene) {
      return true;
    }
  }
  return false;
}

static void setup_app_userdef(BlendFileData *bfd)
{
  if (bfd->user) {
    /* only here free userdef themes... */
    BKE_blender_userdef_data_set_and_free(bfd->user);
    bfd->user = NULL;

    /* Security issue: any blend file could include a USER block.
     *
     * Currently we load prefs from BLENDER_STARTUP_FILE and later on load BLENDER_USERPREF_FILE,
     * to load the preferences defined in the users home dir.
     *
     * This means we will never accidentally (or maliciously)
     * enable scripts auto-execution by loading a '.blend' file.
     */
    U.flag |= USER_SCRIPT_AUTOEXEC_DISABLE;
  }
}

/**
 * Context matching, handle no-ui case
 *
 * \note this is called on Undo so any slow conversion functions here
 * should be avoided or check (mode != LOAD_UNDO).
 *
 * \param bfd: Blend file data, freed by this function on exit.
 * \param filepath: File path or identifier.
 */
static void setup_app_data(bContext *C,
                           BlendFileData *bfd,
                           const char *filepath,
                           const bool is_startup,
                           ReportList *reports)
{
  Main *bmain = G_MAIN;
  Scene *curscene = NULL;
  const bool recover = (G.fileflags & G_FILE_RECOVER) != 0;
  enum {
    LOAD_UI = 1,
    LOAD_UI_OFF,
    LOAD_UNDO,
  } mode;

  /* may happen with library files - UNDO file should never have NULL cursccene... */
  if (ELEM(NULL, bfd->curscreen, bfd->curscene)) {
    BKE_report(reports, RPT_WARNING, "Library file, loading empty scene");
    mode = LOAD_UI_OFF;
  }
  else if (BLI_listbase_is_empty(&bfd->main->screens)) {
    mode = LOAD_UNDO;
  }
  else if ((G.fileflags & G_FILE_NO_UI) && (is_startup == false)) {
    mode = LOAD_UI_OFF;
  }
  else {
    mode = LOAD_UI;
  }

  /* Free all render results, without this stale data gets displayed after loading files */
  if (mode != LOAD_UNDO) {
    RE_FreeAllRenderResults();
  }

  /* Only make filepaths compatible when loading for real (not undo) */
  if (mode != LOAD_UNDO) {
    clean_paths(bfd->main);
  }

  /* XXX here the complex windowmanager matching */

  /* no load screens? */
  if (mode != LOAD_UI) {
    /* Logic for 'track_undo_scene' is to keep using the scene which the active screen has,
     * as long as the scene associated with the undo operation is visible
     * in one of the open windows.
     *
     * - 'curscreen->scene' - scene the user is currently looking at.
     * - 'bfd->curscene' - scene undo-step was created in.
     *
     * This means users can have 2+ windows open and undo in both without screens switching.
     * But if they close one of the screens,
     * undo will ensure that the scene being operated on will be activated
     * (otherwise we'd be undoing on an off-screen scene which isn't acceptable).
     * see: T43424
     */
    wmWindow *win;
    bScreen *curscreen = NULL;
    ViewLayer *cur_view_layer;
    bool track_undo_scene;

    /* comes from readfile.c */
    SWAP(ListBase, bmain->wm, bfd->main->wm);
    SWAP(ListBase, bmain->workspaces, bfd->main->workspaces);
    SWAP(ListBase, bmain->screens, bfd->main->screens);

    /* we re-use current window and screen */
    win = CTX_wm_window(C);
    curscreen = CTX_wm_screen(C);
    /* but use Scene pointer from new file */
    curscene = bfd->curscene;
    cur_view_layer = bfd->cur_view_layer;

    track_undo_scene = (mode == LOAD_UNDO && curscreen && curscene && bfd->main->wm.first);

    if (curscene == NULL) {
      curscene = bfd->main->scenes.first;
    }
    /* empty file, we add a scene to make Blender work */
    if (curscene == NULL) {
      curscene = BKE_scene_add(bfd->main, "Empty");
    }
    if (cur_view_layer == NULL) {
      /* fallback to scene layer */
      cur_view_layer = BKE_view_layer_default_view(curscene);
    }

    if (track_undo_scene) {
      /* keep the old (free'd) scene, let 'blo_lib_link_screen_restore'
       * replace it with 'curscene' if its needed */
    }
    /* and we enforce curscene to be in current screen */
    else if (win) { /* can run in bgmode */
      win->scene = curscene;
    }

    /* BKE_blender_globals_clear will free G_MAIN, here we can still restore pointers */
    blo_lib_link_restore(bmain, bfd->main, CTX_wm_manager(C), curscene, cur_view_layer);
    if (win) {
      curscene = win->scene;
    }

    if (track_undo_scene) {
      wmWindowManager *wm = bfd->main->wm.first;
      if (wm_scene_is_visible(wm, bfd->curscene) == false) {
        curscene = bfd->curscene;
        win->scene = curscene;
        BKE_screen_view3d_scene_sync(curscreen, curscene);
      }
    }

    /* We need to tag this here because events may be handled immediately after.
     * only the current screen is important because we wont have to handle
     * events from multiple screens at once.*/
    if (curscreen) {
      BKE_screen_gizmo_tag_refresh(curscreen);
    }
  }

  /* free G_MAIN Main database */
  //  CTX_wm_manager_set(C, NULL);
  BKE_blender_globals_clear();

  /* clear old property update cache, in case some old references are left dangling */
  RNA_property_update_cache_free();

  bmain = G_MAIN = bfd->main;
  bfd->main = NULL;

  CTX_data_main_set(C, bmain);

  /* case G_FILE_NO_UI or no screens in file */
  if (mode != LOAD_UI) {
    /* leave entire context further unaltered? */
    CTX_data_scene_set(C, curscene);
  }
  else {
    CTX_wm_manager_set(C, bmain->wm.first);
    CTX_wm_screen_set(C, bfd->curscreen);
    CTX_data_scene_set(C, bfd->curscene);
    CTX_wm_area_set(C, NULL);
    CTX_wm_region_set(C, NULL);
    CTX_wm_menu_set(C, NULL);
    curscene = bfd->curscene;
  }

  /* Keep state from preferences. */
  const int fileflags_keep = G_FILE_FLAG_ALL_RUNTIME;
  G.fileflags = (G.fileflags & fileflags_keep) | (bfd->fileflags & ~fileflags_keep);

  /* this can happen when active scene was lib-linked, and doesn't exist anymore */
  if (CTX_data_scene(C) == NULL) {
    wmWindow *win = CTX_wm_window(C);

    /* in case we don't even have a local scene, add one */
    if (!bmain->scenes.first) {
      BKE_scene_add(bmain, "Empty");
    }

    CTX_data_scene_set(C, bmain->scenes.first);
    win->scene = CTX_data_scene(C);
    curscene = CTX_data_scene(C);
  }

  BLI_assert(curscene == CTX_data_scene(C));

  /* special cases, override loaded flags: */
  if (G.f != bfd->globalf) {
    const int flags_keep = G_FLAG_ALL_RUNTIME;
    bfd->globalf &= G_FLAG_ALL_READFILE;
    bfd->globalf = (bfd->globalf & ~flags_keep) | (G.f & flags_keep);
  }

  G.f = bfd->globalf;

#ifdef WITH_PYTHON
  /* let python know about new main */
  BPY_context_update(C);
#endif

  /* FIXME: this version patching should really be part of the file-reading code,
   * but we still get too many unrelated data-corruption crashes otherwise... */
  if (bmain->versionfile < 250) {
    do_versions_ipos_to_animato(bmain);
  }

  bmain->recovered = 0;

  /* startup.blend or recovered startup */
  if (is_startup) {
    bmain->name[0] = '\0';
  }
  else if (recover && G.relbase_valid) {
    /* in case of autosave or quit.blend, use original filename instead
     * use relbase_valid to make sure the file is saved, else we get <memory2> in the filename */
    filepath = bfd->filename;
    bmain->recovered = 1;

    /* these are the same at times, should never copy to the same location */
    if (bmain->name != filepath) {
      BLI_strncpy(bmain->name, filepath, FILE_MAX);
    }
  }

  /* baseflags, groups, make depsgraph, etc */
  /* first handle case if other windows have different scenes visible */
  if (mode == LOAD_UI) {
    wmWindowManager *wm = bmain->wm.first;

    if (wm) {
      for (wmWindow *win = wm->windows.first; win; win = win->next) {
        if (win->scene && win->scene != curscene) {
          BKE_scene_set_background(bmain, win->scene);
        }
      }
    }
  }

  /* Setting scene might require having a dependency graph, with copy on write
   * we need to make sure we ensure scene has correct color management before
   * constructing dependency graph.
   */
  if (mode != LOAD_UNDO) {
    IMB_colormanagement_check_file_config(bmain);
  }

  BKE_scene_set_background(bmain, curscene);

  if (mode != LOAD_UNDO) {
    /* TODO(sergey): Can this be also move above? */
    RE_FreeAllPersistentData();
  }

  if (mode == LOAD_UNDO) {
    /* In undo/redo case, we do a whole lot of magic tricks to avoid having to re-read linked
     * data-blocks from libraries (since those are not supposed to change). Unfortunately, that
     * means that we do not reset their user count, however we do increase that one when doing
     * lib_link on local IDs using linked ones.
     * There is no real way to predict amount of changes here, so we have to fully redo
     * refcounting . */
    BLE_main_id_refcount_recompute(bmain, true);
  }
}

static void setup_app_blend_file_data(bContext *C,
                                      BlendFileData *bfd,
                                      const char *filepath,
                                      const struct BlendFileReadParams *params,
                                      ReportList *reports)
{
  if ((params->skip_flags & BLO_READ_SKIP_USERDEF) == 0) {
    setup_app_userdef(bfd);
  }
  if ((params->skip_flags & BLO_READ_SKIP_DATA) == 0) {
    setup_app_data(C, bfd, filepath, params->is_startup, reports);
  }
}

static int handle_subversion_warning(Main *main, ReportList *reports)
{
  if (main->minversionfile > BLENDER_VERSION ||
      (main->minversionfile == BLENDER_VERSION && main->minsubversionfile > BLENDER_SUBVERSION)) {
    BKE_reportf(reports,
                RPT_ERROR,
                "File written by newer Blender binary (%d.%d), expect loss of data!",
                main->minversionfile,
                main->minsubversionfile);
  }

  return 1;
}

int BKE_blendfile_read(bContext *C,
                       const char *filepath,
                       const struct BlendFileReadParams *params,
                       ReportList *reports)
{
  BlendFileData *bfd;
  bool success = false;

  /* Don't print startup file loading. */
  if (params->is_startup == false) {
    printf("Read blend: %s\n", filepath);
  }

  bfd = BLO_read_from_file(filepath, params->skip_flags, reports);
  if (bfd) {
    if (0 == handle_subversion_warning(bfd->main, reports)) {
      BKE_main_free(bfd->main);
      MEM_freeN(bfd);
      bfd = NULL;
    }
    else {
      setup_app_blend_file_data(C, bfd, filepath, params, reports);
      BLO_blendfiledata_free(bfd);
      success = true;
    }
  }
  else {
    BKE_reports_prependf(reports, "Loading '%s' failed: ", filepath);
  }

  return success;
}

bool BKE_blendfile_read_from_memory(bContext *C,
                                    const void *filebuf,
                                    int filelength,
                                    bool update_defaults,
                                    const struct BlendFileReadParams *params,
                                    ReportList *reports)
{
  BlendFileData *bfd;

  bfd = BLO_read_from_memory(filebuf, filelength, params->skip_flags, reports);
  if (bfd) {
    if (update_defaults) {
      if ((params->skip_flags & BLO_READ_SKIP_DATA) == 0) {
        BLO_update_defaults_startup_blend(bfd->main, NULL);
      }
    }

    setup_app_blend_file_data(C, bfd, "<memory2>", params, reports);
    BLO_blendfiledata_free(bfd);
  }
  else {
    BKE_reports_prepend(reports, "Loading failed: ");
  }

  return (bfd != NULL);
}

/* memfile is the undo buffer */
bool BKE_blendfile_read_from_memfile(bContext *C,
                                     struct MemFile *memfile,
                                     const struct BlendFileReadParams *params,
                                     ReportList *reports)
{
  Main *bmain = CTX_data_main(C);
  BlendFileData *bfd;

  bfd = BLO_read_from_memfile(
      bmain, BKE_main_blendfile_path(bmain), memfile, params->skip_flags, reports);
  if (bfd) {
    /* remove the unused screens and wm */
    while (bfd->main->wm.first) {
      BKE_id_free(bfd->main, bfd->main->wm.first);
    }
    while (bfd->main->screens.first) {
      BKE_id_free(bfd->main, bfd->main->screens.first);
    }

    setup_app_blend_file_data(C, bfd, "<memory1>", params, reports);
    BLO_blendfiledata_free(bfd);
  }
  else {
    BKE_reports_prepend(reports, "Loading failed: ");
  }

  return (bfd != NULL);
}

/**
 * Utility to make a file 'empty' used for startup to optionally give an empty file.
 * Handy for tests.
 */
void BKE_blendfile_read_make_empty(bContext *C)
{
  Main *bmain = CTX_data_main(C);
  ListBase *lb;
  ID *id;

  FOREACH_MAIN_LISTBASE_BEGIN (bmain, lb) {
    FOREACH_MAIN_LISTBASE_ID_BEGIN (lb, id) {
      if (ELEM(GS(id->name), ID_SCE, ID_SCR, ID_WM, ID_WS)) {
        break;
      }
      BKE_id_delete(bmain, id);
    }
    FOREACH_MAIN_LISTBASE_ID_END;
  }
  FOREACH_MAIN_LISTBASE_END;
}

/* only read the userdef from a .blend */
UserDef *BKE_blendfile_userdef_read(const char *filepath, ReportList *reports)
{
  BlendFileData *bfd;
  UserDef *userdef = NULL;

  bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_ALL & ~BLO_READ_SKIP_USERDEF, reports);
  if (bfd) {
    if (bfd->user) {
      userdef = bfd->user;
    }
    BKE_main_free(bfd->main);
    MEM_freeN(bfd);
  }

  return userdef;
}

UserDef *BKE_blendfile_userdef_read_from_memory(const void *filebuf,
                                                int filelength,
                                                ReportList *reports)
{
  BlendFileData *bfd;
  UserDef *userdef = NULL;

  bfd = BLO_read_from_memory(
      filebuf, filelength, BLO_READ_SKIP_ALL & ~BLO_READ_SKIP_USERDEF, reports);
  if (bfd) {
    if (bfd->user) {
      userdef = bfd->user;
    }
    BKE_main_free(bfd->main);
    MEM_freeN(bfd);
  }
  else {
    BKE_reports_prepend(reports, "Loading failed: ");
  }

  return userdef;
}

UserDef *BKE_blendfile_userdef_from_defaults(void)
{
  UserDef *userdef = MEM_mallocN(sizeof(*userdef), __func__);
  memcpy(userdef, &U_default, sizeof(*userdef));

  /* Add-ons. */
  {
    const char *addons[] = {
        "io_anim_bvh",
        "io_curve_svg",
        "io_mesh_ply",
        "io_mesh_stl",
        "io_mesh_uv_layout",
        "io_scene_fbx",
        "io_scene_gltf2",
        "io_scene_obj",
        "io_scene_x3d",
        "cycles",
    };
    for (int i = 0; i < ARRAY_SIZE(addons); i++) {
      bAddon *addon = BKE_addon_new();
      STRNCPY(addon->module, addons[i]);
      BLI_addtail(&userdef->addons, addon);
    }
  }

  /* Theme. */
  {
    bTheme *btheme = MEM_mallocN(sizeof(*btheme), __func__);
    memcpy(btheme, &U_theme_default, sizeof(*btheme));

    BLI_addtail(&userdef->themes, btheme);
  }

#ifdef WITH_PYTHON_SECURITY
  /* use alternative setting for security nuts
   * otherwise we'd need to patch the binary blob - startup.blend.c */
  userdef->flag |= USER_SCRIPT_AUTOEXEC_DISABLE;
#else
  userdef->flag &= ~USER_SCRIPT_AUTOEXEC_DISABLE;
#endif

  /* System-specific fonts directory. */
  BKE_appdir_font_folder_default(userdef->fontdir);

  userdef->memcachelimit = min_ii(BLI_system_memory_max_in_megabytes_int() / 2,
                                  userdef->memcachelimit);

  /* Init weight paint range. */
  BKE_colorband_init(&userdef->coba_weight, true);

  /* Default studio light. */
  BKE_studiolight_default(userdef->light_param, userdef->light_ambient);

  return userdef;
}

/**
 * Only write the userdef in a .blend
 * \return success
 */
bool BKE_blendfile_userdef_write(const char *filepath, ReportList *reports)
{
  Main *mainb = MEM_callocN(sizeof(Main), "empty main");
  bool ok = false;

  if (BLO_write_file(mainb, filepath, G_FILE_USERPREFS, reports, NULL)) {
    ok = true;
  }

  MEM_freeN(mainb);

  return ok;
}

/**
 * Only write the userdef in a .blend, merging with the existing blend file.
 * \return success
 *
 * \note In the future we should re-evaluate user preferences,
 * possibly splitting out system/hardware specific prefs.
 */
bool BKE_blendfile_userdef_write_app_template(const char *filepath, ReportList *reports)
{
  /* if it fails, overwrite is OK. */
  UserDef *userdef_default = BKE_blendfile_userdef_read(filepath, NULL);
  if (userdef_default == NULL) {
    return BKE_blendfile_userdef_write(filepath, reports);
  }

  BKE_blender_userdef_app_template_data_swap(&U, userdef_default);
  bool ok = BKE_blendfile_userdef_write(filepath, reports);
  BKE_blender_userdef_app_template_data_swap(&U, userdef_default);
  BKE_blender_userdef_data_free(userdef_default, false);
  MEM_freeN(userdef_default);
  return ok;
}

bool BKE_blendfile_userdef_write_all(ReportList *reports)
{
  char filepath[FILE_MAX];
  const char *cfgdir;
  bool ok = true;
  const bool use_template_userpref = BKE_appdir_app_template_has_userpref(U.app_template);

  if ((cfgdir = BKE_appdir_folder_id_create(BLENDER_USER_CONFIG, NULL))) {
    bool ok_write;
    BLI_path_join(filepath, sizeof(filepath), cfgdir, BLENDER_USERPREF_FILE, NULL);

    printf("Writing userprefs: '%s' ", filepath);
    if (use_template_userpref) {
      ok_write = BKE_blendfile_userdef_write_app_template(filepath, reports);
    }
    else {
      ok_write = BKE_blendfile_userdef_write(filepath, reports);
    }

    if (ok_write) {
      printf("ok\n");
    }
    else {
      printf("fail\n");
      ok = false;
    }
  }
  else {
    BKE_report(reports, RPT_ERROR, "Unable to create userpref path");
  }

  if (use_template_userpref) {
    if ((cfgdir = BKE_appdir_folder_id_create(BLENDER_USER_CONFIG, U.app_template))) {
      /* Also save app-template prefs */
      BLI_path_join(filepath, sizeof(filepath), cfgdir, BLENDER_USERPREF_FILE, NULL);

      printf("Writing userprefs app-template: '%s' ", filepath);
      if (BKE_blendfile_userdef_write(filepath, reports) != 0) {
        printf("ok\n");
      }
      else {
        printf("fail\n");
        ok = false;
      }
    }
    else {
      BKE_report(reports, RPT_ERROR, "Unable to create app-template userpref path");
      ok = false;
    }
  }

  if (ok) {
    U.runtime.is_dirty = false;
  }
  return ok;
}

WorkspaceConfigFileData *BKE_blendfile_workspace_config_read(const char *filepath,
                                                             const void *filebuf,
                                                             int filelength,
                                                             ReportList *reports)
{
  BlendFileData *bfd;
  WorkspaceConfigFileData *workspace_config = NULL;

  if (filepath) {
    bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_USERDEF, reports);
  }
  else {
    bfd = BLO_read_from_memory(filebuf, filelength, BLO_READ_SKIP_USERDEF, reports);
  }

  if (bfd) {
    workspace_config = MEM_callocN(sizeof(*workspace_config), __func__);
    workspace_config->main = bfd->main;

    /* Only 2.80+ files have actual workspaces, don't try to use screens
     * from older versions. */
    if (bfd->main->versionfile >= 280) {
      workspace_config->workspaces = bfd->main->workspaces;
    }

    MEM_freeN(bfd);
  }

  return workspace_config;
}

bool BKE_blendfile_workspace_config_write(Main *bmain, const char *filepath, ReportList *reports)
{
  int fileflags = G.fileflags & ~(G_FILE_NO_UI | G_FILE_HISTORY);
  bool retval = false;

  BKE_blendfile_write_partial_begin(bmain);

  for (WorkSpace *workspace = bmain->workspaces.first; workspace; workspace = workspace->id.next) {
    BKE_blendfile_write_partial_tag_ID(&workspace->id, true);
  }

  if (BKE_blendfile_write_partial(bmain, filepath, fileflags, reports)) {
    retval = true;
  }

  BKE_blendfile_write_partial_end(bmain);

  return retval;
}

void BKE_blendfile_workspace_config_data_free(WorkspaceConfigFileData *workspace_config)
{
  BKE_main_free(workspace_config->main);
  MEM_freeN(workspace_config);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Partial `.blend` file save.
 * \{ */

void BKE_blendfile_write_partial_begin(Main *bmain_src)
{
  BKE_main_id_tag_all(bmain_src, LIB_TAG_NEED_EXPAND | LIB_TAG_DOIT, false);
}

void BKE_blendfile_write_partial_tag_ID(ID *id, bool set)
{
  if (set) {
    id->tag |= LIB_TAG_NEED_EXPAND | LIB_TAG_DOIT;
  }
  else {
    id->tag &= ~(LIB_TAG_NEED_EXPAND | LIB_TAG_DOIT);
  }
}

static void blendfile_write_partial_cb(void *UNUSED(handle), Main *UNUSED(bmain), void *vid)
{
  if (vid) {
    ID *id = vid;
    /* only tag for need-expand if not done, prevents eternal loops */
    if ((id->tag & LIB_TAG_DOIT) == 0) {
      id->tag |= LIB_TAG_NEED_EXPAND | LIB_TAG_DOIT;
    }

    if (id->lib && (id->lib->id.tag & LIB_TAG_DOIT) == 0) {
      id->lib->id.tag |= LIB_TAG_DOIT;
    }
  }
}

/**
 * \return Success.
 */
bool BKE_blendfile_write_partial(Main *bmain_src,
                                 const char *filepath,
                                 const int write_flags,
                                 ReportList *reports)
{
  Main *bmain_dst = MEM_callocN(sizeof(Main), "copybuffer");
  ListBase *lbarray_dst[MAX_LIBARRAY], *lbarray_src[MAX_LIBARRAY];
  int a, retval;

  void *path_list_backup = NULL;
  const int path_list_flag = (BKE_BPATH_TRAVERSE_SKIP_LIBRARY | BKE_BPATH_TRAVERSE_SKIP_MULTIFILE);

  /* This is needed to be able to load that file as a real one later
   * (otherwise main->name will not be set at read time). */
  BLI_strncpy(bmain_dst->name, bmain_src->name, sizeof(bmain_dst->name));

  BLO_main_expander(blendfile_write_partial_cb);
  BLO_expand_main(NULL, bmain_src);

  /* move over all tagged blocks */
  set_listbasepointers(bmain_src, lbarray_src);
  a = set_listbasepointers(bmain_dst, lbarray_dst);
  while (a--) {
    ID *id, *nextid;
    ListBase *lb_dst = lbarray_dst[a], *lb_src = lbarray_src[a];

    for (id = lb_src->first; id; id = nextid) {
      nextid = id->next;
      if (id->tag & LIB_TAG_DOIT) {
        BLI_remlink(lb_src, id);
        BLI_addtail(lb_dst, id);
      }
    }
  }

  /* Backup paths because remap relative will overwrite them.
   *
   * NOTE: we do this only on the list of data-blocks that we are writing
   * because the restored full list is not guaranteed to be in the same
   * order as before, as expected by BKE_bpath_list_restore.
   *
   * This happens because id_sort_by_name does not take into account
   * string case or the library name, so the order is not strictly
   * defined for two linked data-blocks with the same name! */
  if (write_flags & G_FILE_RELATIVE_REMAP) {
    path_list_backup = BKE_bpath_list_backup(bmain_dst, path_list_flag);
  }

  /* save the buffer */
  retval = BLO_write_file(bmain_dst, filepath, write_flags, reports, NULL);

  if (path_list_backup) {
    BKE_bpath_list_restore(bmain_dst, path_list_flag, path_list_backup);
    BKE_bpath_list_free(path_list_backup);
  }

  /* move back the main, now sorted again */
  set_listbasepointers(bmain_src, lbarray_dst);
  a = set_listbasepointers(bmain_dst, lbarray_src);
  while (a--) {
    ID *id;
    ListBase *lb_dst = lbarray_dst[a], *lb_src = lbarray_src[a];

    while ((id = BLI_pophead(lb_src))) {
      BLI_addtail(lb_dst, id);
      id_sort_by_name(lb_dst, id);
    }
  }

  MEM_freeN(bmain_dst);

  return retval;
}

void BKE_blendfile_write_partial_end(Main *bmain_src)
{
  BKE_main_id_tag_all(bmain_src, LIB_TAG_NEED_EXPAND | LIB_TAG_DOIT, false);
}

/** \} */
