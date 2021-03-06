
#include <AMReX_BLFort.H>
#include <Castro.H>
#include <Castro_bc_fill_nd.H>
#include <Castro_bc_fill_nd_F.H>
#include <Castro_bc_ext_fill_nd.H>
#include <Castro_generic_fill.H>
#include <Castro_generic_fill_F.H>

using namespace amrex;

void ca_statefill(Box const& bx, FArrayBox& data,
                  const int dcomp, const int numcomp,
                  Geometry const& geom, const Real time,
                  const Vector<BCRec>& bcr, const int bcomp,
                  const int scomp)
{
    // Make a copy of the raw BCRec data in the format
    // our BC routines can handle (a contiguous array
    // of integers).

    Vector<int> bcrs(2 * AMREX_SPACEDIM * numcomp);

    for (int n = 0; n < numcomp; ++n)
        for (int k = 0; k < 2 * AMREX_SPACEDIM; ++k)
            bcrs[2 * AMREX_SPACEDIM * n + k] = bcr[n].vect()[k];

#ifdef AMREX_USE_CUDA
    int* bc_f = prepare_bc(bcrs.data(), numcomp);
    set_bc_launch_config();
#else
    const int* bc_f = bcrs.data();
#endif

    // This routine either comes in with one component or all NUM_STATE.

    if (numcomp == 1) {

#pragma gpu box(bx)
        denfill(AMREX_INT_ANYD(bx.loVect()), AMREX_INT_ANYD(bx.hiVect()),
                BL_TO_FORTRAN_N_ANYD(data, dcomp),
                AMREX_INT_ANYD(geom.Domain().loVect()), AMREX_INT_ANYD(geom.Domain().hiVect()),
                AMREX_REAL_ANYD(geom.CellSize()), AMREX_REAL_ANYD(geom.ProbLo()), time, bc_f);

    }
    else {

        AMREX_ALWAYS_ASSERT(numcomp == NUM_STATE);

#pragma gpu box(bx)
        hypfill(AMREX_INT_ANYD(bx.loVect()), AMREX_INT_ANYD(bx.hiVect()),
                BL_TO_FORTRAN_ANYD(data),
                AMREX_INT_ANYD(geom.Domain().loVect()), AMREX_INT_ANYD(geom.Domain().hiVect()),
                AMREX_REAL_ANYD(geom.CellSize()), AMREX_REAL_ANYD(geom.ProbLo()), time, bc_f);

    }

#ifdef AMREX_USE_CUDA
    clean_bc_launch_config();
#endif

    if (numcomp == 1) {

        ca_ext_denfill(bx, data, dcomp, numcomp, geom, time, bc_f);

    }
    else {

        AMREX_ALWAYS_ASSERT(numcomp == NUM_STATE);

        ca_ext_fill(bx, data, dcomp, numcomp, geom, time, bc_f);

    }

#ifdef AMREX_USE_CUDA
    clean_bc(bc_f);
#endif
}
