#define BLINN

vec3 spherical_harmonics(vec3 N, vec3 spherical_harmonics_coefs[STUDIOLIGHT_SPHERICAL_HARMONICS_MAX_COMPONENTS])
{
	vec3 sh = vec3(0.0);

	sh += 0.282095 * spherical_harmonics_coefs[0];

#if STUDIOLIGHT_SPHERICAL_HARMONICS_LEVEL > 0
	sh += -0.488603 * N.z * spherical_harmonics_coefs[1];
	sh += 0.488603 * N.y * spherical_harmonics_coefs[2];
	sh += -0.488603 * N.x * spherical_harmonics_coefs[3];
#endif

#if STUDIOLIGHT_SPHERICAL_HARMONICS_LEVEL > 1
	sh += 1.092548 * N.x * N.z * spherical_harmonics_coefs[4];
	sh += -1.092548 * N.z * N.y * spherical_harmonics_coefs[5];
	sh += 0.315392 * (3.0 * N.y * N.y - 1.0) * spherical_harmonics_coefs[6];
	sh += -1.092548 * N.x * N.y * spherical_harmonics_coefs[7];
	sh += 0.546274 * (N.x * N.x - N.z * N.z) * spherical_harmonics_coefs[8];
#endif

	return sh;
}

vec3 get_world_diffuse_light(WorldData world_data, vec3 N)
{
	return (spherical_harmonics(vec3(N.x, N.y, -N.z), world_data.spherical_harmonics_coefs));
}

vec3 get_camera_diffuse_light(WorldData world_data, vec3 N)
{
	return (spherical_harmonics(vec3(N.x, -N.z, -N.y), world_data.spherical_harmonics_coefs));
}

/* N And I are in View Space. */
vec3 get_world_specular_light(vec4 specular_data, LightData light_data, vec3 N, vec3 I)
{
#ifdef V3D_SHADING_SPECULAR_HIGHLIGHT
	vec3 specular_light = specular_data.rgb * light_data.specular_color.rgb * light_data.specular_color.a;

	float shininess = exp2(10*(1.0-specular_data.a) + 1);

#  ifdef BLINN
	float normalization_factor = (shininess + 8) / (8 * M_PI);
	vec3 L = -light_data.light_direction_vs.xyz;
	vec3 halfDir = normalize(L + I);
	float specAngle = max(dot(halfDir, N), 0.0);
	float NL = max(dot(L, N), 0.0);
	float specular_influence = pow(specAngle, shininess) * NL  * normalization_factor;

#  else
	vec3 reflection_vector = reflect(I, N);
	float specAngle = max(dot(light_data.light_direction_vs.xyz, reflection_vector), 0.0);
	float specular_influence = pow(specAngle, shininess);
#  endif

	vec3 specular_color = specular_light * specular_influence;

#else /* V3D_SHADING_SPECULAR_HIGHLIGHT */
	vec3 specular_color = vec3(0.0);
#endif /* V3D_SHADING_SPECULAR_HIGHLIGHT */
	return specular_color;
}

vec3 get_world_specular_lights(WorldData world_data, vec4 specular_data, vec3 N, vec3 I)
{
	vec3 specular_light = vec3(0.0);
	specular_light += get_world_specular_light(specular_data, world_data.lights[0], N, I);
	for (int i = 0 ; i < world_data.num_lights ; i ++) {
		specular_light += get_world_specular_light(specular_data, world_data.lights[i], N, I);
	}
	return specular_light;
}