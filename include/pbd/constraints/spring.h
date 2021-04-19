#pragma once

/// \file
/// Spring constraints.

#include "pbd/common.h"
#include "pbd/math/matrix.h"
#include "pbd/math/vector.h"
#include "pbd/body.h"

namespace pbd::constraints {
	/// Properties of a spring constraint.
	struct spring_constraint_properties {
		/// No initialization.
		spring_constraint_properties(uninitialized_t) {
		}

		double length; ///< The length of this spring.
		double inverse_stiffness; ///< The inverse stiffness of this spring.
	};

	/// A constraint between two particles that follows the Hooke's law.
	struct particle_spring {
		/// No initialization.
		particle_spring(uninitialized_t) {
		}

		/// Projects this constraint.
		void project(cvec3d &x1, cvec3d &x2, double inv_m1, double inv_m2, double inv_dt2, double &lambda) const {
			cvec3d t = x2 - x1;
			double t_len = t.norm();
			double t_diff = t_len - properties.length;
			double c = t_diff;
			double inv_k_dt2 = properties.inverse_stiffness * inv_dt2;
			double delta_lambda = -(c + inv_k_dt2 * lambda) / (inv_m1 + inv_m2 + inv_k_dt2);
			lambda += delta_lambda;
			cvec3d dx = (delta_lambda / t_len) * t;
			x1 -= inv_m1 * dx;
			x2 += inv_m2 * dx;
		}

		spring_constraint_properties properties = uninitialized; ///< Properties of this constraint.
		std::size_t particle1; ///< The first particle affected by this constraint.
		std::size_t particle2; ///< The second particle affected by this constraint.
	};

	/// A contact constraint between two bodies.
	struct body_contact {
		/// No initialization.
		body_contact(uninitialized_t) {
		}
		/// Creates a contact for the given bodies at the given contact position in world space.
		[[nodiscard]] inline static body_contact create_for(body &b1, body &b2, cvec3d p1, cvec3d p2, cvec3d n) {
			body_contact result = uninitialized;
			result.offset1 = b1.state.rotation.inverse().rotate(p1 - b1.state.position);
			result.offset2 = b2.state.rotation.inverse().rotate(p2 - b2.state.position);
			result.normal = n;
			result.body1 = &b1;
			result.body2 = &b2;
			return result;
		}

		/// Projects this constraint.
		void project(double inv_dt2, double &lambda) {
			cvec3d global1 = body1->state.position + body1->state.rotation.rotate(offset1);
			cvec3d global2 = body2->state.position + body2->state.rotation.rotate(offset2);
			double depth = vec::dot(global1 - global2, normal);
			if (depth < 0.0) {
				return;
			}

			cvec3d n1 = body1->state.rotation.inverse().rotate(normal);
			cvec3d n2 = body2->state.rotation.inverse().rotate(normal);

			cvec3d rot1 = vec::cross(offset1, n1);
			cvec3d rot2 = vec::cross(offset2, n2);
			double w1 = body1->properties.inverse_mass + vec::dot(rot1, body1->properties.inverse_inertia * rot1);
			double w2 = body2->properties.inverse_mass + vec::dot(rot2, body2->properties.inverse_inertia * rot2);

			double delta_lambda = -depth / (w1 + w2);
			lambda += delta_lambda;
			cvec3d p = normal * delta_lambda;
			body1->state.position += p * body1->properties.inverse_mass;
			body2->state.position -= p * body2->properties.inverse_mass;
			cvec3d p1 = n1 * delta_lambda;
			cvec3d p2 = n2 * delta_lambda;
			cvec3d rot_vec1 = body1->state.rotation.rotate(body1->properties.inverse_inertia * vec::cross(offset1, p1));
			cvec3d rot_vec2 = body2->state.rotation.rotate(body2->properties.inverse_inertia * vec::cross(offset2, p2));
			quatd new_rot1 = body1->state.rotation + 0.5 * quatd::from_vector(rot_vec1) * body1->state.rotation;
			quatd new_rot2 = body2->state.rotation - 0.5 * quatd::from_vector(rot_vec2) * body2->state.rotation;
			body1->state.rotation = quat::unsafe_normalize(new_rot1);
			body2->state.rotation = quat::unsafe_normalize(new_rot2);

			force = lambda * normal * inv_dt2;
		}

		/// Offset of the spring's connection to \ref body1 in its local coordinates.
		cvec3d offset1 = uninitialized;
		/// Offset of the spring's connection to \ref body2 in its local coordinates.
		cvec3d offset2 = uninitialized;
		cvec3d normal = uninitialized; ///< Contact normal.
		cvec3d force = uninitialized; ///< Contact force.
		body *body1; ///< The first body.
		body *body2; ///< The second body.
	};

	/// A constraint between two bodies that follows the Hooke's law.
	struct body_spring {
		/// No initialization.
		body_spring(uninitialized_t) {
		}

		spring_constraint_properties properties = uninitialized; ///< Properties of this constraint.
		/// Offset of the spring's connection to \ref body1 in its local coordinates.
		cvec3d offset1 = uninitialized;
		/// Offset of the spring's connection to \ref body2 in its local coordinates.
		cvec3d offset2 = uninitialized;
		body *body1; ///< The first body.
		body *body2; ///< The second body.
	};
}
