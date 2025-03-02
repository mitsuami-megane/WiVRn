/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "wivrn_hmd.h"
#include "wivrn_session.h"

#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"

#include "util/u_device.h"
#include "util/u_logging.h"
#include "utils/method.h"

#include "xrt_cast.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdio.h>
#include <openxr/openxr.h>

#include "configuration.h"

namespace wivrn
{

static void wivrn_hmd_destroy(xrt_device * xdev);

static double foveate(double a, double b, double λ, double c, double x)
{
	// In order to save encoding, transmit and decoding time, only a portion of the image is encoded in full resolution.
	// on each axis, foveated coordinates are defined by the following formula.
	return λ / a * tan(a * x + b) + c;
	// a and b are defined such as:
	// edges of the image are not moved
	// f(-1) = -1
	// f( 1) =  1
	// the function also enforces pixel ratio 1:1 at fovea
	// df⁻¹(x)/dx = 1/scale for x = c
}

static double foveate_lod(double a, double b, double /*λ*/, double /*c*/, double x)
{
	// derivate of foveate * scale
	// bias it to favor sharper image
	return std::max(0., log2(1 / (cos(a * x + b) * cos(a * x + b)) - 0.5));
}

static std::tuple<float, float> solve_foveation(float λ, float c)
{
	// Compute a and b for the foveation function such that:
	//   foveate(a, b, scale, c, -1) = -1   (eq. 1)
	//   foveate(a, b, scale, c,  1) =  1   (eq. 2)
	//
	// Use eq. 2 to express a as function of b, then replace in eq. 1
	// equation that needs to be null is:
	auto b = [λ, c](double a) { return atan(a * (1 - c) / λ) - a; };
	auto eq = [λ, c](double a) { return atan(a * (1 - c) / λ) + atan(a * (1 + c) / λ) - 2 * a; }; // (eq. 3)

	// function starts positive, reaches a maximum then decreases to -∞
	double a0 = 0;
	// Find a negative value by computing eq(2^n)
	double a1 = 1;
	while (eq(a1) > 0)
		a1 *= 2;

	// last computed values for f(a0) and f(a1)
	std::optional<double> f_a0;
	double f_a1 = eq(a1);

	int n = 0;
	double a = 0;
	while (std::abs(a1 - a0) > 0.0000001 && n++ < 100)
	{
		if (not f_a0)
		{
			// use binary search
			a = 0.5 * (a0 + a1);
			double val = eq(a);
			if (val > 0)
			{
				a0 = a;
				f_a0 = val;
			}
			else
			{
				a1 = a;
				f_a1 = val;
			}
		}
		else
		{
			// f(a1) is always defined
			// when f(a0) is defined, use secant method
			a = a1 - f_a1 * (a1 - a0) / (f_a1 - *f_a0);
			a0 = a1;
			a1 = a;
			f_a0 = f_a1;
			f_a1 = eq(a);
		}
	}

	return {a, b(a)};
}

bool wivrn_hmd::wivrn_hmd_compute_distortion(xrt_device * xdev, uint32_t view_index, float u, float v, xrt_uv_triplet * result)
{
	// u,v are in the output coordinates (sent to the encoder)
	// result is in the input coordinates (from the application)
	const auto & param = ((wivrn_hmd *)xdev)->foveation_parameters[view_index];
	xrt_vec2 out;
	xrt_vec2 lod;

	if (param.x.scale < 1)
	{
		u = 2 * u - 1;

		out.x = foveate(param.x.a, param.x.b, param.x.scale, param.x.center, u);
		lod.x = foveate_lod(param.x.a, param.x.b, param.x.scale, param.x.center, u);
		out.x = std::clamp<float>((1 + out.x) / 2, 0, 1);
	}
	else
	{
		out.x = u;
		lod.x = 0;
	}

	if (param.y.scale < 1)
	{
		v = 2 * v - 1;

		out.y = foveate(param.y.a, param.y.b, param.y.scale, param.y.center, v);
		lod.y = foveate_lod(param.y.a, param.y.b, param.y.scale, param.y.center, v);
		out.y = std::clamp<float>((1 + out.y) / 2, 0, 1);
	}
	else
	{
		out.y = v;
		lod.y = 0;
	}

	U_LOG_D("distortion parameters: %f %f (%f %f)", lod.x, lod.y, u, v);

	result->r = out;
	result->g = lod;
	result->b = out;

	return true;
}

xrt_result_t wivrn_hmd::get_visibility_mask(xrt_device * xdev, xrt_visibility_mask_type type, uint32_t view_index, xrt_visibility_mask ** mask)
{
	static_assert(sizeof(uint32_t) == sizeof(decltype(from_headset::visibility_mask_changed::mask::indices)::value_type));
	static_assert(sizeof(xrt_vec2) == sizeof(decltype(from_headset::visibility_mask_changed::mask::vertices)::value_type));
	auto self = (wivrn_hmd *)xdev;
	type = xrt_visibility_mask_type((unsigned int)type - 1); // enum values start at 1
	const auto visibility_mask = self->visibility_mask.lock();
	if (type >= from_headset::visibility_mask_changed::num_types or view_index >= 2 or not(*visibility_mask)[view_index])
	{
		*mask = (xrt_visibility_mask *)calloc(1, sizeof(xrt_visibility_mask));
		return XRT_SUCCESS;
	}
	const auto & in_mask = (*(*visibility_mask)[view_index])[int(type)];
	size_t index_size = in_mask.indices.size() * sizeof(uint32_t);
	size_t vertex_size = in_mask.vertices.size() * sizeof(xrt_vec2);
	*mask = (xrt_visibility_mask *)calloc(1, sizeof(xrt_visibility_mask) + index_size + vertex_size);
	**mask = {
	        .type = type,
	        .index_count = uint32_t(in_mask.indices.size()),
	        .vertex_count = uint32_t(in_mask.vertices.size()),
	};
	memcpy(xrt_visibility_mask_get_indices(*mask), in_mask.indices.data(), index_size);
	memcpy(xrt_visibility_mask_get_vertices(*mask), in_mask.vertices.data(), vertex_size);
	return XRT_SUCCESS;
}

wivrn_hmd::wivrn_hmd(wivrn::wivrn_session * cnx,
                     const from_headset::headset_info_packet & info) :
        xrt_device{}, cnx(cnx)
{
	xrt_device * base = this;

	base->hmd = &hmd_parts;
	base->tracking_origin = &tracking_origin;

	base->update_inputs = [](xrt_device *) { return XRT_SUCCESS; };
	base->get_tracked_pose = method_pointer<&wivrn_hmd::get_tracked_pose>;
	base->get_view_poses = method_pointer<&wivrn_hmd::get_view_poses>;
	base->get_battery_status = method_pointer<&wivrn_hmd::get_battery_status>;
	base->get_visibility_mask = get_visibility_mask;
	base->destroy = wivrn_hmd_destroy;
	name = XRT_DEVICE_GENERIC_HMD;
	device_type = XRT_DEVICE_TYPE_HMD;
	orientation_tracking_supported = true;
	// hand_tracking_supported = true;
	battery_status_supported = true;
	position_tracking_supported = true;

	// Print name.
	strcpy(str, "WiVRn HMD");
	strcpy(serial, "WiVRn HMD");

	// Setup input.
	pose_input.name = XRT_INPUT_GENERIC_HEAD_POSE;
	pose_input.active = true;
	inputs = &pose_input;
	input_count = 1;

	const auto config = configuration::read_user_configuration();

	auto eye_width = info.recommended_eye_width;
	eye_width = ((eye_width + 3) / 4) * 4;
	auto eye_height = info.recommended_eye_height;
	eye_height = ((eye_height + 3) / 4) * 4;

	// Setup info.
	hmd->view_count = 2;
	hmd->blend_modes[hmd->blend_mode_count++] = XRT_BLEND_MODE_OPAQUE;
	if (info.passthrough)
		hmd->blend_modes[hmd->blend_mode_count++] = XRT_BLEND_MODE_ALPHA_BLEND;

	hmd->distortion.models = XRT_DISTORTION_MODEL_NONE;
	hmd->distortion.preferred = XRT_DISTORTION_MODEL_NONE;
	hmd->screens[0].w_pixels = eye_width * 2;
	hmd->screens[0].h_pixels = eye_height;
	hmd->screens[0].nominal_frame_interval_ns = 1000000000 / info.preferred_refresh_rate;

	// Left
	hmd->views[0].display.w_pixels = eye_width;
	hmd->views[0].display.h_pixels = eye_height;
	hmd->views[0].rot = u_device_rotation_ident;

	// Right
	hmd->views[1].display.w_pixels = eye_width;
	hmd->views[1].display.h_pixels = eye_height;
	hmd->views[1].rot = u_device_rotation_ident;

	// FOV from headset info packet
	hmd->distortion.fov[0] = xrt_cast(info.fov[0]);
	hmd->distortion.fov[1] = xrt_cast(info.fov[1]);
}

xrt_result_t wivrn_hmd::get_tracked_pose(xrt_input_name name, int64_t at_timestamp_ns, xrt_space_relation * res)
{
	if (name != XRT_INPUT_GENERIC_HEAD_POSE)
	{
		U_LOG_E("Unknown input name");
		return {};
	}

	auto [extrapolation_time, view] = views.get_at(at_timestamp_ns);
	*res = view.relation;
	cnx->add_predict_offset(extrapolation_time);
	return XRT_SUCCESS;
}

void wivrn_hmd::update_tracking(const from_headset::tracking & tracking, const clock_offset & offset)
{
	views.update_tracking(tracking, offset);
}

void wivrn_hmd::update_battery(const from_headset::battery & new_battery)
{
	// We will only request a new sample if the current one is consumed
	cnx->set_enabled(to_headset::tracking_control::id::battery, false);
	std::lock_guard lock(mutex);
	battery = new_battery;
}

void wivrn_hmd::get_view_poses(const xrt_vec3 * default_eye_relation,
                               int64_t at_timestamp_ns,
                               uint32_t view_count,
                               xrt_space_relation * out_head_relation,
                               xrt_fov * out_fovs,
                               xrt_pose * out_poses)
{
	auto [extrapolation_time, view] = views.get_at(at_timestamp_ns);
	cnx->add_predict_offset(extrapolation_time);

	int flags = view.relation.relation_flags;

	if (not(view.flags & XR_VIEW_STATE_POSITION_VALID_BIT))
		flags &= ~XRT_SPACE_RELATION_POSITION_VALID_BIT;

	if (not(view.flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
		flags &= ~XRT_SPACE_RELATION_ORIENTATION_VALID_BIT;

	if (not(view.flags & XR_VIEW_STATE_POSITION_TRACKED_BIT))
		flags &= ~XRT_SPACE_RELATION_POSITION_TRACKED_BIT;

	if (not(view.flags & XR_VIEW_STATE_ORIENTATION_TRACKED_BIT))
		flags &= ~XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT;

	view.relation.relation_flags = (xrt_space_relation_flags)flags;
	*out_head_relation = view.relation;

	assert(view_count == 2);
	for (size_t eye = 0; eye < 2; ++eye)
	{
		out_fovs[eye] = view.fovs[eye];
		out_poses[eye] = view.poses[eye];
	}
}

xrt_result_t wivrn_hmd::get_battery_status(bool * out_present,
                                           bool * out_charging,
                                           float * out_charge)
{
	cnx->set_enabled(to_headset::tracking_control::id::battery, true);

	std::lock_guard lock(mutex);
	*out_present = battery.present;
	*out_charging = battery.charging;
	*out_charge = battery.charge;

	return XRT_SUCCESS;
}

decltype(wivrn_hmd::foveation_parameters) wivrn_hmd::set_foveated_size(uint32_t width, uint32_t height)
{
	assert(width % 2 == 0);
	uint32_t eye_width = width / 2;
	std::array<double, 2> scale{
	        double(eye_width) / hmd->views[0].display.w_pixels,
	        double(height) / hmd->views[0].display.h_pixels,
	};

	hmd->screens[0].w_pixels = width;
	hmd->screens[0].h_pixels = height;

	for (int i = 0; i < 2; ++i)
	{
		auto & view = hmd->views[i];
		view.viewport.x_pixels = i * eye_width;
		view.viewport.y_pixels = 0;

		view.viewport.w_pixels = eye_width;
		view.viewport.h_pixels = height;

		foveation_parameters[i].x.scale = scale[0];
		foveation_parameters[i].y.scale = scale[1];

		const auto & fov = hmd->distortion.fov[i];
		float l = tan(fov.angle_left);
		float r = tan(fov.angle_right);
		float t = tan(fov.angle_up);
		float b = tan(fov.angle_down);
		if (scale[0] < 1)
		{
			float cu = (r + l) / (l - r);
			foveation_parameters[i].x.center = cu;

			std::tie(foveation_parameters[i].x.a, foveation_parameters[i].x.b) = solve_foveation(scale[0], cu);
		}

		if (scale[1] < 1)
		{
			float cv = (t + b) / (t - b);
			foveation_parameters[i].y.center = cv;

			std::tie(foveation_parameters[i].y.a, foveation_parameters[i].y.b) = solve_foveation(scale[1], cv);
		}
	}

	// Distortion information
	compute_distortion = wivrn_hmd_compute_distortion;
	hmd->distortion.models = XRT_DISTORTION_MODEL_COMPUTE;
	hmd->distortion.preferred = XRT_DISTORTION_MODEL_COMPUTE;
	return foveation_parameters;
}

void wivrn_hmd::set_foveation_center(std::array<xrt_vec2, 2> center)
{
	for (int i = 0; i < 2; ++i)
	{
		foveation_parameters[i].x.center = center[i].x;
		foveation_parameters[i].y.center = center[i].y;

		if (foveation_parameters[i].x.scale < 1)
		{
			std::tie(foveation_parameters[i].x.a, foveation_parameters[i].x.b) =
			        solve_foveation(foveation_parameters[i].x.scale, foveation_parameters[i].x.center);
		}
		if (foveation_parameters[i].y.scale < 1)
		{
			std::tie(foveation_parameters[i].y.a, foveation_parameters[i].y.b) =
			        solve_foveation(foveation_parameters[i].y.scale, foveation_parameters[i].y.center);
		}
	}
}

void wivrn_hmd::update_visibility_mask(const from_headset::visibility_mask_changed & mask)
{
	assert(mask.view_index < 2);
	auto m = visibility_mask.lock();
	m->at(mask.view_index) = mask.data;
}

/*
 *
 * Functions
 *
 */

static void wivrn_hmd_destroy(xrt_device * xdev)
{
	static_cast<wivrn_hmd *>(xdev)->unregister();
}
} // namespace wivrn
