//
// SPDX-License-Identifier: MIT
// Copyright (C) 2020 - 2022 by the ryujin authors
//

#pragma once

namespace ryujin
{
  namespace EquationOfStateLibrary
  {
    /**
     * The Noble-Abel-Stiffened gas equation of state
     *
     * @ingroup EquationOfState
     */
    class NobleAbleStiffenedGas : public EquationOfState
    {
    public:
      NobleAbleStiffenedGas(const std::string subsection)
          : EquationOfState("noble-able-stiffened gas", subsection)
      {
        gamma_ = 7. / 5.;
        this->add_parameter("gamma", gamma_, "The ratio of specific heats");

        b_ = 0.;
        this->add_parameter(
            "covolume b", b_, "The maximum compressibility constant");

        q_ = 0.;
        this->add_parameter(
            "reference sie q", q_, "The reference specific internal energy");

        pinf_ = 0.;
        this->add_parameter(
            "reference pressure", pinf_, "The reference pressure p infinity");
      }

      /* Pressure oracle */
      virtual double
      pressure_oracle(const double rho,
                      const double internal_energy) final override
      {
        /* p = (\gamma - 1) *  (\rho (e - q))/ (1 - b \rho) - \gamma p_\infty */

        const auto cov = 1. - b_ * rho;

        const auto num = internal_energy - q_ * rho;
        const auto den = cov;
        return (gamma_ - 1.) * num / den - gamma_ * pinf_;
      }

      /* Sie from rho and p */
      virtual double sie_from_rho_p(const double rho,
                                    const double pressure) final override
      {
        const auto cov = 1. - b_ * rho;

        const auto num = pressure + gamma_ * pinf_;
        const auto den = gamma_ - 1.;

        return num / den * rho * cov + q_;
      }

    private:
      double gamma_;
      double b_;
      double q_;
      double pinf_;
    };
  } // namespace EquationOfStateLibrary
} /* namespace ryujin */
