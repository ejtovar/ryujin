#ifndef POSTPROCESSOR_TEMPLATE_H
#define POSTPROCESSOR_TEMPLATE_H

#include "helper.h"
#include "postprocessor.h"

#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/vector_tools.h>

#include <atomic>
#include <fstream>

namespace grendel
{
  using namespace dealii;

  template <>
  const std::array<std::string, 2> Postprocessor<1, double>::component_names{
      "schlieren", "alpha"};

  template <>
  const std::array<std::string, 3> Postprocessor<2, double>::component_names{
      "schlieren", "vorticity", "alpha"};

  template <>
  const std::array<std::string, 3> Postprocessor<3, double>::component_names{
      "schlieren", "vorticity", "alpha"};

  template <>
  const std::array<std::string, 2> Postprocessor<1, float>::component_names{
      "schlieren", "alpha"};

  template <>
  const std::array<std::string, 3> Postprocessor<2, float>::component_names{
      "schlieren", "vorticity", "alpha"};

  template <>
  const std::array<std::string, 3> Postprocessor<3, float>::component_names{
      "schlieren", "vorticity", "alpha"};


  template <int dim, typename Number>
  Postprocessor<dim, Number>::Postprocessor(
      const MPI_Comm &mpi_communicator,
      const grendel::OfflineData<dim, Number> &offline_data,
      const std::string &subsection /*= "Postprocessor"*/)
      : ParameterAcceptor(subsection)
      , mpi_communicator_(mpi_communicator)
      , offline_data_(&offline_data)
  {
    schlieren_beta_ = 10.;
    add_parameter(
        "schlieren beta",
        schlieren_beta_,
        "Beta factor used in the exponential scale for the schlieren plot");

    vorticity_beta_ = 10.;
    add_parameter(
        "vorticity beta",
        vorticity_beta_,
        "Beta factor used in the exponential scale for the vorticity");

    output_full_ = true;
    add_parameter("output full", output_full_, "Output the full mesh");

    add_parameter(
        "output planes",
        output_planes_,
        "A vector of hyperplanes described by an origin, normal vector and a "
        "tolerance. If nonempty, only cells intersecting with the plane will "
        "be written out to disc. Example declaration of two hyper planes in "
        "3D, one normal to the x-axis and one normal to the y-axis: \"0,0,0 : "
        "1,0,0 : 0.01 ; 0,0,0 : 0,1,0 : 0,01\"");

#if 0
    coarsening_level_ = 0;
    add_parameter(
        "coarsening level",
        coarsening_level_,
        "Number of coarsening steps applied before writing pvtu/vtu output");
#endif
  }


  template <int dim, typename Number>
  void Postprocessor<dim, Number>::prepare()
  {
#ifdef DEBUG_OUTPUT
    deallog << "Postprocessor<dim, Number>::prepare()" << std::endl;
#endif

    const auto &partitioner = offline_data_->partitioner();

    for (auto &it : U_)
      it.reinit(partitioner);

    for (auto &it : quantities_)
      it.reinit(partitioner);

#if 0
    if (coarsening_level_ != 0) {
      /* Prepare MGTransferMatrixFree for coarsened output: */

      const auto &dof_handler = offline_data_->dof_handler();
      const auto &triangulation = dof_handler.get_triangulation();

      const auto max_level = triangulation.n_global_levels() - 1;
      const auto output_level =
          max_level - std::min(coarsening_level_, max_level);

      constrained_dofs_.initialize(dof_handler);

      transfer_.initialize_constraints(constrained_dofs_);
      transfer_.build(dof_handler);

      for (auto &it : output_U_)
        it.resize(output_level, max_level);

      for (auto &it : output_quantities_)
        it.resize(output_level, max_level);
    }
#endif
  }


  template <int dim, typename Number>
  void Postprocessor<dim, Number>::compute(const vector_type &U,
                                           const scalar_type &alpha)
  {
#ifdef DEBUG_OUTPUT
    deallog << "Postprocessor<dim, Number>::compute()" << std::endl;
#endif

    constexpr auto n_array_elements = VectorizedArray<Number>::n_array_elements;

    const auto &affine_constraints = offline_data_->affine_constraints();
    const auto &sparsity_simd = offline_data_->sparsity_pattern_simd();
    const auto &lumped_mass_matrix = offline_data_->lumped_mass_matrix();
    const auto &cij_matrix = offline_data_->cij_matrix();
    const auto &boundary_normal_map = offline_data_->boundary_normal_map();

    const unsigned int n_internal = offline_data_->n_locally_internal();
    const unsigned int n_locally_owned = offline_data_->n_locally_owned();

    /*
     * Step 1: Copy the current state vector over to output_vector:
     */

    for (unsigned int i = 0; i < problem_dimension; ++i) {
      U_[i] = U[i];
    }

    /*
     * Step 2: Compute r_i and r_i_max, r_i_min:
     */

    std::atomic<Number> r_i_max{0.};
    std::atomic<Number> r_i_min{std::numeric_limits<Number>::infinity()};
    std::atomic<Number> v_i_max{0.};
    std::atomic<Number> v_i_min{std::numeric_limits<Number>::infinity()};

    {
      GRENDEL_PARALLEL_REGION_BEGIN

      Number r_i_max_on_subrange = 0.;
      Number r_i_min_on_subrange = std::numeric_limits<Number>::infinity();
      Number v_i_max_on_subrange = 0.;
      Number v_i_min_on_subrange = std::numeric_limits<Number>::infinity();

      GRENDEL_OMP_FOR
      for (unsigned int i = 0; i < n_locally_owned; ++i) {

        const unsigned int row_length = sparsity_simd.row_length(i);

        /* Skip constrained degrees of freedom */
        if (row_length == 1)
          continue;

        Tensor<1, dim, Number> grad_rho_i;
        curl_type curl_M_i;

        /* Skip diagonal. */
        const unsigned int *js = sparsity_simd.columns(i);
        for (unsigned int col_idx = 1; col_idx < row_length; ++col_idx) {
          const auto j = *(i < n_internal ? js + col_idx * n_array_elements
                                          : js + col_idx);

          const auto U_j = gather(U, j);
          const auto M_j = ProblemDescription<dim, Number>::momentum(U_j);

          const auto c_ij = cij_matrix.get_tensor(i, col_idx);

          grad_rho_i += c_ij * U_j[0];

          if constexpr (dim == 2) {
            curl_M_i[0] += cross_product_2d(c_ij) * M_j;
          } else if constexpr (dim == 3) {
            curl_M_i += cross_product_3d(c_ij, M_j);
          }
        }

        /* Fix up boundaries: */

        const auto bnm_it = boundary_normal_map.find(i);
        if (bnm_it != boundary_normal_map.end()) {
          const auto [normal, id, _] = bnm_it->second;
          /* FIXME: Think again about what to do exactly here... */
          if (id == Boundary::slip) {
            grad_rho_i -= 1. * (grad_rho_i * normal) * normal;
          } else {
            grad_rho_i = 0.;
          }
          curl_M_i = 0.;
        }

        /* Populate quantities: */

        const Number rho_i = U[0].local_element(i);
        const Number m_i = lumped_mass_matrix.local_element(i);

        Tensor<1, n_quantities, Number> quantities;

        quantities[0] = grad_rho_i.norm() / m_i;
        if constexpr (dim > 1) {
          quantities[1] = curl_M_i.norm() / (m_i * rho_i);
        }
        quantities[n_quantities - 1] = alpha.local_element(i);

        r_i_max_on_subrange = std::max(r_i_max_on_subrange, quantities[0]);
        r_i_min_on_subrange = std::min(r_i_min_on_subrange, quantities[0]);
        if constexpr (dim > 1) {
          v_i_max_on_subrange = std::max(v_i_max_on_subrange, quantities[1]);
          v_i_min_on_subrange = std::min(v_i_min_on_subrange, quantities[1]);
        }

        scatter(quantities_, quantities, i);
      }

      /* Synchronize over all threads: */

      Number temp = r_i_max.load();
      while (temp < r_i_max_on_subrange &&
             !r_i_max.compare_exchange_weak(temp, r_i_max_on_subrange))
        ;
      temp = r_i_min.load();
      while (temp > r_i_min_on_subrange &&
             !r_i_min.compare_exchange_weak(temp, r_i_min_on_subrange))
        ;

      temp = v_i_max.load();
      while (temp < v_i_max_on_subrange &&
             !v_i_max.compare_exchange_weak(temp, v_i_max_on_subrange))
        ;
      temp = v_i_min.load();
      while (temp > v_i_min_on_subrange &&
             !v_i_min.compare_exchange_weak(temp, v_i_min_on_subrange))
        ;

      GRENDEL_PARALLEL_REGION_END
    }

    /* And synchronize over all processors: */

    r_i_max.store(Utilities::MPI::max(r_i_max.load(), mpi_communicator_));
    r_i_min.store(Utilities::MPI::min(r_i_min.load(), mpi_communicator_));
    v_i_max.store(Utilities::MPI::max(v_i_max.load(), mpi_communicator_));
    v_i_min.store(Utilities::MPI::min(v_i_min.load(), mpi_communicator_));

    /*
     * Step 3: Normalize schlieren and vorticity:
     */

    {
      GRENDEL_PARALLEL_REGION_BEGIN

      GRENDEL_OMP_FOR
      for (unsigned int i = 0; i < n_locally_owned; ++i) {

        const unsigned int row_length = sparsity_simd.row_length(i);

        /* Skip constrained degrees of freedom */
        if (row_length == 1)
          continue;

        auto &r_i = quantities_[0].local_element(i);
        r_i = Number(1.) - std::exp(-schlieren_beta_ * (r_i - r_i_min) /
                                    (r_i_max - r_i_min));

        if constexpr (dim > 1) {
          auto &v_i = quantities_[1].local_element(i);
          v_i = Number(1.) - std::exp(-vorticity_beta_ * (v_i - v_i_min) /
                                      (v_i_max - v_i_min));
        }
      }

      GRENDEL_PARALLEL_REGION_END
    }

    /*
     * Step 4: Fix up constraints and distribute:
     */

    for (auto &it : U_) {
      affine_constraints.distribute(it);
      it.update_ghost_values();
    }

    for (auto &it : quantities_) {
      affine_constraints.distribute(it);
      it.update_ghost_values();
    }

#if 0
    /*
     * Step 5: interpolate to coarse mesh
     */

    if (coarsening_level_ != 0) {
      const auto &dof_handler = offline_data_->dof_handler();

      for (unsigned int i = 0; i < problem_dimension; ++i)
        transfer_.interpolate_to_mg(dof_handler, output_U_[i], U_[i]);

      for (unsigned int i = 0; i < n_quantities; ++i)
        transfer_.interpolate_to_mg(
            dof_handler, output_quantities_[i], quantities_[i]);
    }
#endif
  }


  template <int dim, typename Number>
  void Postprocessor<dim, Number>::write_out_vtu(std::string name,
                                                 Number t,
                                                 unsigned int cycle)
  {
    constexpr auto problem_dimension =
        ProblemDescription<dim, Number>::problem_dimension;
    constexpr auto n_quantities = Postprocessor<dim, Number>::n_quantities;

    const auto &discretization = offline_data_->discretization();
    const auto &mapping = discretization.mapping();

    dealii::DataOut<dim> data_out;

    data_out.attach_dof_handler(offline_data_->dof_handler());

    for (unsigned int i = 0; i < problem_dimension; ++i)
      data_out.add_data_vector(
          U_[i], ProblemDescription<dim, Number>::component_names[i]);
    for (unsigned int i = 0; i < n_quantities; ++i)
      data_out.add_data_vector(quantities_[i], component_names[i]);

    DataOutBase::VtkFlags flags(
        t, cycle, true, DataOutBase::VtkFlags::best_speed);
    data_out.set_flags(flags);

    const auto patch_order = discretization.finite_element().degree - 1;

    if (output_full_) {
      data_out.build_patches(mapping, patch_order);
      data_out.write_vtu_with_pvtu_record(
          "", name, cycle, mpi_communicator_, 6);
    }

    if (output_planes_.size() != 0) {
      /*
       * Specify an output filter that selects only cells for output that are
       * in the viscinity of a specified set of output planes:
       */

      data_out.set_cell_selection([this](const auto &cell) {
        if (!cell->is_active() || cell->is_artificial())
          return false;

        if (output_planes_.size() == 0)
          return true;

        for (const auto &plane : output_planes_) {
          const auto &[origin, normal, tolerance] = plane;

          unsigned int above = 0;
          unsigned int below = 0;

          for (unsigned int v = 0; v < GeometryInfo<dim>::vertices_per_cell;
               ++v) {
            const auto vertex = cell->vertex(v);
            const auto distance = (vertex - origin) * normal;
            if (distance > -tolerance)
              above++;
            if (distance < tolerance)
              below++;
            if (above > 0 && below > 0)
              return true;
          }
        }
        return false;
      });

      data_out.build_patches(mapping, patch_order);
      data_out.write_vtu_with_pvtu_record(
          "", name + "-cut_planes", cycle, mpi_communicator_, 6);
    }
  }

} /* namespace grendel */

#endif /* POSTPROCESSOR_TEMPLATE_H */
