//
// SPDX-License-Identifier: MIT
// Copyright (C) 2020 - 2021 by the ryujin authors
//

#pragma once

#include <compile_time_options.h>

#include "offline_data.h"
#include "problem_description.h"

#include <deal.II/distributed/solution_transfer.h>

namespace ryujin
{

  /**
   * A solution transfer class that interpolates a given state U (in
   * conserved variables) to a refined/coarsened mesh. The class first
   * transforms the conserved state into primitive variables and then
   * interpolates/restricts the primitive state field via deal.II's
   * SolutionTransfer mechanism.
   *
   * @ingroup TimeLoop
   */
  template <int dim, typename Number = double>
  class SolutionTransfer final
  {
  public:
    /**
     * @copydoc ProblemDescription::problem_dimension
     */
    // clang-format off
    static constexpr unsigned int problem_dimension = ProblemDescription::problem_dimension<dim>;
    // clang-format on

    /**
     * @copydoc ProblemDescription::state_type
     */
    using state_type = ProblemDescription::state_type<dim, Number>;

    /**
     * @copydoc OfflineData::scalar_type
     */
    using scalar_type = typename OfflineData<dim, Number>::scalar_type;

    /**
     * @copydoc OfflineData::vector_type
     */
    using vector_type = typename OfflineData<dim, Number>::vector_type;

    /**
     * Constructor.
     */
    SolutionTransfer(const ryujin::OfflineData<dim, Number> &offline_data,
                     const ryujin::ProblemDescription &problem_description)
        : offline_data_(&offline_data)
        , problem_description_(&problem_description)
        , solution_transfer_(offline_data.dof_handler())
    {
    }

    /**
     * Read in a state vector (in conserved quantities). The function
     * populates an auxiliary distributed vectors that store the given
     * state in primitive variables and then calls the underlying deal.II
     * SolutionTransfer::prepare_for_coarsening_and_refinement();
     *
     * This function has to be called before the actual grid refinement.
     */
    void prepare_for_interpolation(const vector_type &U)
    {
      const auto &scalar_partitioner = offline_data_->scalar_partitioner();
      const auto &affine_constraints = offline_data_->affine_constraints();

      state_.resize(problem_dimension);
      for (auto &it : state_)
        it.reinit(scalar_partitioner);

      const unsigned int n_owned = offline_data_->n_locally_owned();

      /* copy over the primitive state: */

      for (unsigned int i = 0; i < n_owned; ++i) {
        const auto U_i = U.get_tensor(i);
        const auto primitive_state =
            problem_description_->to_primitive_state(U_i);

        for(unsigned int k = 0; k < problem_dimension; ++k)
          state_[k].local_element(i) = primitive_state[k];
      }

      for (auto &it : state_) {
        affine_constraints.distribute(it);
        it.update_ghost_values();
      }

      std::vector<const scalar_type *> ptr_state;
      std::transform(state_.begin(),
                     state_.end(),
                     std::back_inserter(ptr_state),
                     [](auto &it) { return &it; });
      solution_transfer_.prepare_for_coarsening_and_refinement(ptr_state);
    }

    /**
     * Finalize the state vector transfer by calling
     * SolutionTransfer::interpolate() and repopulating the state vector
     * (in conserved quantities).
     *
     * This function has to be called after the actual grid refinement.
     */
    void interpolate(vector_type &U)
    {
      const auto &scalar_partitioner = offline_data_->scalar_partitioner();

      U.reinit(offline_data_->vector_partitioner());

      interpolated_state_.resize(problem_dimension);
      for (auto &it : interpolated_state_) {
        it.reinit(scalar_partitioner);
#if DEAL_II_VERSION_GTE(9,3,0)
        it.zero_out_ghost_values();
#else
        it.zero_out_ghosts();
#endif
      }

      std::vector<scalar_type *> ptr_interpolated_state;
      std::transform(interpolated_state_.begin(),
                     interpolated_state_.end(),
                     std::back_inserter(ptr_interpolated_state),
                     [](auto &it) { return &it; });
      solution_transfer_.interpolate(ptr_interpolated_state);

      const unsigned int n_owned = offline_data_->n_locally_owned();

      /* copy over primitive_state: */

      for (unsigned int i = 0; i < n_owned; ++i) {
        state_type U_i;
        for(unsigned int k = 0; k < problem_dimension; ++k)
          U_i[k] = interpolated_state_[k].local_element(i);
        U_i = problem_description_->from_primitive_state(U_i);

        U.write_tensor(U_i, i);
      }

      U.update_ghost_values();
    }

  private:
    //@}
    /**
     * @name Internal data
     */
    //@{
    dealii::SmartPointer<const ryujin::OfflineData<dim, Number>> offline_data_;
    dealii::SmartPointer<const ryujin::ProblemDescription> problem_description_;

    dealii::parallel::distributed::SolutionTransfer<dim, scalar_type>
        solution_transfer_;

    std::vector<scalar_type> state_;
    std::vector<scalar_type> interpolated_state_;
    //@}
  };

} /* namespace ryujin */
