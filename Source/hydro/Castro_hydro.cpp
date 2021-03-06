#include "Castro.H"
#include "Castro_F.H"
#include "Castro_hydro_F.H"

#ifdef RADIATION
#include "Radiation.H"
#endif

using namespace amrex;

void
Castro::cons_to_prim(const Real time)
{

    BL_PROFILE("Castro::cons_to_prim()");
    
#ifdef RADIATION
    AmrLevel::FillPatch(*this, Erborder, NUM_GROW, time, Rad_Type, 0, Radiation::nGroups);

    MultiFab lamborder(grids, dmap, Radiation::nGroups, NUM_GROW);
    if (radiation->pure_hydro) {
      lamborder.setVal(0.0, NUM_GROW);
    }
    else {
      radiation->compute_limiter(level, grids, Sborder, Erborder, lamborder);
    }
#endif

    MultiFab& S_new = get_new_data(State_Type);

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(S_new, hydro_tile_size); mfi.isValid(); ++mfi) {

        const Box& qbx = mfi.growntilebox(NUM_GROW);

        Array4<Real> const q_arr = q.array(mfi);
        Array4<Real> const qaux_arr = qaux.array(mfi);
        Array4<Real> const Sborder_arr = Sborder.array(mfi);
#ifdef RADIATION
        Array4<Real> const Erborder_arr = Erborder.array(mfi);
        Array4<Real> const lamborder_arr = lamborder.array(mfi);
#endif

        // Convert the conservative state to the primitive variable state.
        // This fills both q and qaux.

        ctoprim(qbx,
                time,
                Sborder_arr,
#ifdef RADIATION
                Erborder_arr,
                lamborder_arr,
#endif
                q_arr,
                qaux_arr);

        // Convert the source terms expressed as sources to the conserved state to those
        // expressed as sources for the primitive state.
        if (time_integration_method == CornerTransportUpwind ||
            time_integration_method == SimplifiedSpectralDeferredCorrections) {

          Array4<Real> const src_arr = sources_for_hydro.array(mfi);
          Array4<Real> const src_q_arr = src_q.array(mfi);

          src_to_prim(qbx, q_arr, qaux_arr, src_arr, src_q_arr);
        }

#ifndef RADIATION
#ifdef SIMPLIFIED_SDC
#ifdef REACTIONS
        // Add in the reactions source term; only done in simplified SDC.

        if (time_integration_method == SimplifiedSpectralDeferredCorrections) {

            MultiFab& SDC_react_source = get_new_data(Simplified_SDC_React_Type);

            if (do_react)
                src_q[mfi].plus<RunOn::Device>(SDC_react_source[mfi],qbx,qbx,0,0,NQSRC);

        }
#endif
#endif
#endif
    }

}

// Convert a MultiFab with conservative state data u to a primitive MultiFab q.
void
Castro::cons_to_prim(MultiFab& u, MultiFab& q_in, MultiFab& qaux_in, Real time)
{

    BL_PROFILE("Castro::cons_to_prim()");

    BL_ASSERT(u.nComp() == NUM_STATE);
    BL_ASSERT(q_in.nComp() == NQ);
    BL_ASSERT(u.nGrow() >= q_in.nGrow());

    int ng = q_in.nGrow();

#ifdef RADIATION
    AmrLevel::FillPatch(*this, Erborder, NUM_GROW, time, Rad_Type, 0, Radiation::nGroups);

    MultiFab lamborder(grids, dmap, Radiation::nGroups, NUM_GROW);
    if (radiation->pure_hydro) {
      lamborder.setVal(0.0, NUM_GROW);
    }
    else {
      radiation->compute_limiter(level, grids, Sborder, Erborder, lamborder);
    }
#endif

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(u, TilingIfNotGPU()); mfi.isValid(); ++mfi) {

        const Box& bx = mfi.growntilebox(ng);

        auto u_arr = u.array(mfi);
#ifdef RADIATION
        auto Erborder_arr = Erborder.array(mfi);
        auto lamborder_arr = lamborder.array(mfi);
#endif
        auto q_in_arr = q_in.array(mfi);
        auto qaux_in_arr = qaux_in.array(mfi);

        ctoprim(bx,
                time,
                u_arr,
#ifdef RADIATION
                Erborder_arr,
                lamborder_arr,
#endif
                q_in_arr,
                qaux_in_arr);

    }

}

#ifdef TRUE_SDC
void
Castro::cons_to_prim_fourth(const Real time)
{

    BL_PROFILE("Castro::cons_to_prim_fourth()");
    
    // convert the conservative state cell averages to primitive cell
    // averages with 4th order accuracy

    const int* domain_lo = geom.Domain().loVect();
    const int* domain_hi = geom.Domain().hiVect();

    MultiFab& S_new = get_new_data(State_Type);

    // we don't support radiation here
#ifdef RADIATION
    amrex::Abort("radiation not supported to fourth order");
#else
#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(S_new, hydro_tile_size); mfi.isValid(); ++mfi) {

      const Box& qbx = mfi.growntilebox(NUM_GROW);
      const Box& qbxm1 = mfi.growntilebox(NUM_GROW-1);

      // note: these conversions are using a growntilebox, so it
      // will include ghost cells

      // convert U_avg to U_cc -- this will use a Laplacian
      // operation and will result in U_cc defined only on
      // NUM_GROW-1 ghost cells at the end.
      FArrayBox U_cc;
      U_cc.resize(qbx, NUM_STATE);

      ca_make_cell_center(BL_TO_FORTRAN_BOX(qbxm1),
                          BL_TO_FORTRAN_FAB(Sborder[mfi]),
                          BL_TO_FORTRAN_FAB(U_cc),
                          AMREX_ARLIM_ANYD(domain_lo), AMREX_ARLIM_ANYD(domain_hi));

      // enforce the minimum density on the new cell-centered state
      Real dens_change = 1.e0;
      ca_enforce_minimum_density
        (AMREX_ARLIM_ANYD(qbxm1.loVect()), AMREX_ARLIM_ANYD(qbxm1.hiVect()),
         BL_TO_FORTRAN_ANYD(U_cc),
         &dens_change, verbose);

      // and ensure that the internal energy is positive
      reset_internal_energy(qbxm1, U_cc.array());

      // convert U_avg to q_bar -- this will be done on all NUM_GROW
      // ghost cells.
      auto Sborder_arr = Sborder.array(mfi);
      auto q_bar_arr = q_bar.array(mfi);
      auto qaux_bar_arr = qaux_bar.array(mfi);

      ctoprim(qbx,
              time, 
              Sborder_arr,
              q_bar_arr,
              qaux_bar_arr);

      // this is what we should construct the flattening coefficient
      // from

      // convert U_cc to q_cc (we'll store this temporarily in q,
      // qaux).  This will remain valid only on the NUM_GROW-1 ghost
      // cells.
      auto U_cc_arr = U_cc.array();
      auto q_arr = q.array(mfi);
      auto qaux_arr = qaux.array(mfi);

      ctoprim(qbxm1,
              time,
              U_cc_arr,
              q_arr,
              qaux_arr);
    }


    // check for NaNs
#ifndef AMREX_USE_CUDA
    check_for_nan(q);
    check_for_nan(q_bar);
#endif

#ifdef DIFFUSION
    // we need the cell-center temperature for the diffusion stencil,
    // so save it here, by copying from q (which is cell-center at the
    // moment).
    MultiFab::Copy(T_cc, q, QTEMP, 0, 1, NUM_GROW-1);
#endif

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(S_new, hydro_tile_size); mfi.isValid(); ++mfi) {

      const Box& qbxm1 = mfi.growntilebox(NUM_GROW-1);

      // now convert q, qaux into 4th order accurate averages
      // this will create q, qaux in NUM_GROW-1 ghost cells, but that's
      // we need here

      ca_make_fourth_average(BL_TO_FORTRAN_BOX(qbxm1),
                             BL_TO_FORTRAN_FAB(q[mfi]),
                             BL_TO_FORTRAN_FAB(q_bar[mfi]),
                             AMREX_ARLIM_ANYD(domain_lo), AMREX_ARLIM_ANYD(domain_hi));

      // not sure if we need to convert qaux this way, or if we can
      // just evaluate it (we may not need qaux at all actually)
      ca_make_fourth_average(BL_TO_FORTRAN_BOX(qbxm1),
                             BL_TO_FORTRAN_FAB(qaux[mfi]),
                             BL_TO_FORTRAN_FAB(qaux_bar[mfi]),
                             AMREX_ARLIM_ANYD(domain_lo), AMREX_ARLIM_ANYD(domain_hi));

    }

#endif // RADIATION
}
#endif

void
Castro::check_for_cfl_violation(const Real dt)
{

    BL_PROFILE("Castro::check_for_cfl_violation()");

    Real courno = -1.0e+200;

    const Real *dx = geom.CellSize();

    MultiFab& S_new = get_new_data(State_Type);

#ifdef _OPENMP
#pragma omp parallel reduction(max:courno)
#endif
    for (MFIter mfi(S_new, hydro_tile_size); mfi.isValid(); ++mfi) {

        const Box& bx = mfi.tilebox();

#pragma gpu box(bx)
        ca_compute_cfl(BL_TO_FORTRAN_BOX(bx),
                       BL_TO_FORTRAN_ANYD(q[mfi]),
                       BL_TO_FORTRAN_ANYD(qaux[mfi]),
                       dt, AMREX_REAL_ANYD(dx), AMREX_MFITER_REDUCE_MAX(&courno), print_fortran_warnings);

    }

    ParallelDescriptor::ReduceRealMax(courno);

    if (courno > 1.0) {
        amrex::Print() << "WARNING -- EFFECTIVE CFL AT LEVEL " << level << " IS " << courno << std::endl << std::endl;

        cfl_violation = 1;
    }

}
