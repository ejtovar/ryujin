//
// SPDX-License-Identifier: MIT
// Copyright (C) 2020 - 2022 by the ryujin authors
//

#pragma once

#include <compile_time_options.h>

#include "riemann_solver.h"

#include <newton.h>
#include <simd.h>

namespace ryujin
{
  namespace EulerAEOS
  {
    /*
     * The RiemannSolver is a guaranteed maximal wavespeed (GMS) estimate
     * for the extended Riemann problem outlined in
     * @cite ClaytonGuermondPopov-2022.
     *
     * In contrast to the algorithm outlined in above reference the
     * algorithm takes a couple of shortcuts to significantly decrease the
     * computational footprint. These simplifications still guarantee that
     * we have an upper bound on the maximal wavespeed - but the number
     * bound might be larger. In particular:
     *
     *  - We will assume that the nonvaccum condition holds true, i.e.,
     *    phi(0) < 0.
     *
     *  - We do not check and treat the case phi(p_min) > 0. This
     *    corresponds to two expansion waves, see §5.2 in the reference. In
     *    this case we have
     *
     *      0 < p_star < p_min <= p_max.
     *
     *    And due to the fact that p_star < p_min the wavespeeds reduce to
     *    a left wavespeed v_L - a_L and right wavespeed v_R + a_R. This
     *    implies that it is sufficient to set p_2 to ANY value provided
     *    that p_2 <= p_min hold true in order to compute the correct
     *    wavespeed.
     *
     *    If p_2 > p_min then a more pessimistic bound is computed.
     */

    template <int dim, typename Number>
    DEAL_II_ALWAYS_INLINE inline Number RiemannSolver<dim, Number>::alpha(
        const Number &rho, const Number &gamma, const Number &a) const
    {
      using ScalarNumber = typename get_value_type<Number>::type;

      const ScalarNumber b_interp = hyperbolic_system.b_interp();

      const Number numerator =
          ScalarNumber(2.) * a * (Number(1.) - b_interp * rho);

      const Number denominator = gamma - Number(1.);

      return numerator / denominator;
    }


    template <int dim, typename Number>
    DEAL_II_ALWAYS_INLINE inline Number
    RiemannSolver<dim, Number>::c(const Number gamma_Z) const
    {
      using ScalarNumber = typename get_value_type<Number>::type;

      Number radicand = (ScalarNumber(3.) * gamma_Z + Number(11.)) /
                        (ScalarNumber(6.) * (gamma_Z + Number(1.)));
      const Number false_value = std::sqrt(radicand);

      Number c_of_gamma = dealii::compare_and_apply_mask<
          dealii::SIMDComparison::less_than_or_equal>(
          gamma_Z, Number(5. / 3.), Number(1.), false_value);

      const Number true_value = Number(0.5 * std::sqrt(2.));

      c_of_gamma = dealii::compare_and_apply_mask<
          dealii::SIMDComparison::greater_than_or_equal>(
          gamma_Z, Number(3.), true_value, c_of_gamma);

      return c_of_gamma;
    }


    template <int dim, typename Number>
    DEAL_II_ALWAYS_INLINE inline Number
    RiemannSolver<dim, Number>::p_star_RS_aeos(
        const primitive_type &riemann_data_i,
        const primitive_type &riemann_data_j) const
    {
      using ScalarNumber = typename get_value_type<Number>::type;

      const auto &[rho_i, u_i, p_i, gamma_i, a_i] = riemann_data_i;
      const auto &[rho_j, u_j, p_j, gamma_j, a_j] = riemann_data_j;
      const auto alpha_i = alpha(rho_i, gamma_i, a_i);
      const auto alpha_j = alpha(rho_j, gamma_j, a_j);

      /*
       * First get p_min, p_max.
       *
       * Then, we get gamma_min/max, and alpha_min/max. Note that the
       * *_min/max values are associated with p_min/max and are not
       * necessarily the minimum/maximum of *_i vs *_j.
       */

      const Number p_min = std::min(p_i, p_j);
      const Number p_max = std::max(p_i, p_j);

      const Number gamma_min =
          dealii::compare_and_apply_mask<dealii::SIMDComparison::less_than>(
              p_i, p_j, gamma_i, gamma_j);

      const Number gamma_max = dealii::compare_and_apply_mask<
          dealii::SIMDComparison::greater_than_or_equal>(
          p_i, p_j, gamma_i, gamma_j);

      const Number alpha_min =
          dealii::compare_and_apply_mask<dealii::SIMDComparison::less_than>(
              p_i, p_j, alpha_i, alpha_j);

      const Number alpha_max = dealii::compare_and_apply_mask<
          dealii::SIMDComparison::greater_than_or_equal>(
          p_i, p_j, alpha_i, alpha_j);

      const Number c_gamma_min = c(gamma_min);

      const Number exp_min =
          ScalarNumber(2.) * gamma_min / (gamma_min - Number(1.));
      const Number exp_max =
          (gamma_max - Number(1.)) / (ScalarNumber(2.) * gamma_max);

      /* Then we can compute p_star_RS */
      const Number numerator =
          alpha_max * (Number(1.) - ryujin::vec_pow(p_min / p_max, exp_max)) -
          (u_j - u_i);

      const Number denominator = c_gamma_min * alpha_min;

      const Number base = numerator / denominator + Number(1.);

      return p_min * ryujin::vec_pow(base, exp_min);
    }


    template <int dim, typename Number>
    DEAL_II_ALWAYS_INLINE inline Number
    RiemannSolver<dim, Number>::p_star_SS_aeos(
        const primitive_type &riemann_data_i,
        const primitive_type &riemann_data_j) const
    {
      using ScalarNumber = typename get_value_type<Number>::type;

      const auto &[rho_i, u_i, p_i, gamma_i, a_i] = riemann_data_i;
      const auto &[rho_j, u_j, p_j, gamma_j, a_j] = riemann_data_j;

      const Number gamma_m = std::min(gamma_i, gamma_j);

      /* Compute alpha_hat_left and alpha_hat_right  */
      const Number alpha_hat_left = c(gamma_i) * alpha(rho_i, gamma_i, a_i);
      const Number alpha_hat_right = c(gamma_j) * alpha(rho_j, gamma_j, a_j);

      const Number exp = (gamma_m - Number(1.)) / (ScalarNumber(2.) * gamma_m);
      const Number exp_inv = Number(1.) / exp;

      /* Then we can compute p_star_SS */
      const Number numerator = alpha_hat_left + alpha_hat_right - (u_j - u_i);

      const Number denominator = alpha_hat_left * ryujin::vec_pow(p_i, -exp) +
                                 alpha_hat_right * ryujin::vec_pow(p_j, -exp);

      const Number base = numerator / denominator;

      return ryujin::vec_pow(base, exp_inv);
    }


    template <int dim, typename Number>
    DEAL_II_ALWAYS_INLINE inline Number
    RiemannSolver<dim, Number>::phi_of_p_max(
        const primitive_type &riemann_data_i,
        const primitive_type &riemann_data_j) const
    {
      using ScalarNumber = typename get_value_type<Number>::type;
      const ScalarNumber b_interp = hyperbolic_system.b_interp();

      const auto &[rho_i, u_i, p_i, gamma_i, a_i] = riemann_data_i;
      const auto &[rho_j, u_j, p_j, gamma_j, a_j] = riemann_data_j;

      const Number p_max = std::max(p_i, p_j);

      const Number radicand_inverse_i =
          ScalarNumber(0.5) * rho_i / (Number(1.) - b_interp * rho_i) *
          ((gamma_i + Number(1.)) * p_max + (gamma_i - Number(1.)) * p_i);

      const Number value_i = (p_max - p_i) / std::sqrt(radicand_inverse_i);

      const Number radicand_jnverse_j =
          ScalarNumber(0.5) * rho_j / (Number(1.) - b_interp * rho_j) *
          ((gamma_j + Number(1.)) * p_max + (gamma_j - Number(1.)) * p_j);

      const Number value_j = (p_max - p_j) / std::sqrt(radicand_jnverse_j);

      return value_i + value_j + u_j - u_i;
    }


    template <int dim, typename Number>
    DEAL_II_ALWAYS_INLINE inline Number
    RiemannSolver<dim, Number>::lambda1_minus(
        const primitive_type &riemann_data, const Number p_star) const
    {
      const auto &[rho, u, p, gamma, a] = riemann_data;

      const auto factor =
          ScalarNumber(0.5) * (gamma + ScalarNumber(1.)) / gamma;

      const Number tmp = positive_part((p_star - p) / p);

      return u - a * std::sqrt(Number(1.) + factor * tmp);
    }


    template <int dim, typename Number>
    DEAL_II_ALWAYS_INLINE inline Number
    RiemannSolver<dim, Number>::lambda3_plus(const primitive_type &riemann_data,
                                             const Number p_star) const
    {
      const auto &[rho, u, p, gamma, a] = riemann_data;

      const auto factor =
          ScalarNumber(0.5) * (gamma + ScalarNumber(1.)) / gamma;

      const Number tmp = positive_part((p_star - p) / p);

      return u + a * std::sqrt(Number(1.) + factor * tmp);
    }


    template <int dim, typename Number>
    DEAL_II_ALWAYS_INLINE inline Number
    RiemannSolver<dim, Number>::compute_lambda(
        const primitive_type &riemann_data_i,
        const primitive_type &riemann_data_j,
        const Number p_star) const
    {
      const Number nu_11 = lambda1_minus(riemann_data_i, p_star);
      const Number nu_32 = lambda3_plus(riemann_data_j, p_star);

      return std::max(positive_part(nu_32), negative_part(nu_11));
    }


    template <int dim, typename Number>
    Number RiemannSolver<dim, Number>::compute(
        const primitive_type &riemann_data_i,
        const primitive_type &riemann_data_j) const
    {
      const auto &[rho_i, u_i, p_i, gamma_i, a_i] = riemann_data_i;
      const auto &[rho_j, u_j, p_j, gamma_j, a_j] = riemann_data_j;

      const Number p_max = std::max(p_i, p_j);
      const Number phi_p_max = phi_of_p_max(riemann_data_i, riemann_data_j);

      const Number p_star_SS = p_star_SS_aeos(riemann_data_i, riemann_data_j);

      const Number p_star_RS = p_star_RS_aeos(riemann_data_i, riemann_data_j);

      const Number p_2 =
          dealii::compare_and_apply_mask<dealii::SIMDComparison::less_than>(
              phi_p_max, Number(0.), p_star_SS, std::min(p_max, p_star_RS));

      return compute_lambda(riemann_data_i, riemann_data_j, p_2);
    }

  } // namespace EulerAEOS
} // namespace ryujin
