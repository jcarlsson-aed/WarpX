#include "BoundaryConditions/WarpX_PEC.H"

#include "WarpX.H"

#include <AMReX_Box.H>
#include <AMReX_Geometry.H>
#include <AMReX_GpuControl.H>
#include <AMReX_GpuLaunch.H>
#include <AMReX_IndexType.H>
#include <AMReX_MFIter.H>
#include <AMReX_MultiFab.H>
#include <AMReX_SPACE.H>
#include <AMReX_Vector.H>

using namespace amrex::literals;

bool
PEC::isAnyBoundaryPEC() {
    for (int idim = 0; idim < AMREX_SPACEDIM; ++idim) {
        if ( WarpX::field_boundary_lo[idim] == FieldBoundaryType::PEC) return true;
        if ( WarpX::field_boundary_hi[idim] == FieldBoundaryType::PEC) return true;
    }
    return false;
}

void
PEC::ApplyPECtoEfield (std::array<amrex::MultiFab*, 3> Efield, const int lev,
                       PatchType patch_type, const bool split_pml_field)
{
    auto& warpx = WarpX::GetInstance();
    amrex::Box domain_box = warpx.Geom(lev).Domain();
    if (patch_type == PatchType::coarse) {
        amrex::IntVect ref_ratio = ( (lev > 0) ? WarpX::RefRatio(lev-1) : amrex::IntVect(1) );
        domain_box.coarsen(ref_ratio);
    }
    amrex::IntVect domain_lo = domain_box.smallEnd();
    amrex::IntVect domain_hi = domain_box.bigEnd();
    amrex::GpuArray<int, 3> fbndry_lo;
    amrex::GpuArray<int, 3> fbndry_hi;
    for (int idim=0; idim < AMREX_SPACEDIM; ++idim) {
        fbndry_lo[idim] = WarpX::field_boundary_lo[idim];
        fbndry_hi[idim] = WarpX::field_boundary_hi[idim];
    }
    amrex::IntVect Ex_nodal = Efield[0]->ixType().toIntVect();
    amrex::IntVect Ey_nodal = Efield[1]->ixType().toIntVect();
    amrex::IntVect Ez_nodal = Efield[2]->ixType().toIntVect();
    amrex::IntVect ng_fieldgather = warpx.get_ng_fieldgather();
    // For each Efield multifab, apply PEC boundary condition to ncomponents
    // If not split E-field, the PEC is applied to the regular Efield used in Maxwell's eq.
    // If split_pml_field is true, then PEC is applied to all the split field components of the tangential field.
    int nComp_x = Efield[0]->nComp();
    int nComp_y = Efield[1]->nComp();
    int nComp_z = Efield[2]->nComp();
#ifdef AMREX_USE_OMP
#pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
#endif
    for (amrex::MFIter mfi(*Efield[0], amrex::TilingIfNotGPU()); mfi.isValid(); ++mfi) {
        // Extract field data
        amrex::Array4<amrex::Real> const& Ex = Efield[0]->array(mfi);
        amrex::Array4<amrex::Real> const& Ey = Efield[1]->array(mfi);
        amrex::Array4<amrex::Real> const& Ez = Efield[2]->array(mfi);

        // Extract tileboxes for which to loop
        // if split field, the box includes nodal flag
        // For E-field used in Maxwell's update, nodal flag plus cells that particles
        // gather fields from in the guard-cell region are included.
        // Note that for simulations without particles or laser, ng_field_gather is 0
        // and the guard-cell values of the E-field multifab will not be modified.
        amrex::Box const& tex = (split_pml_field) ? mfi.tilebox(Efield[0]->ixType().toIntVect())
                                                  : mfi.tilebox(Efield[0]->ixType().toIntVect(), ng_fieldgather);
        amrex::Box const& tey = (split_pml_field) ? mfi.tilebox(Efield[1]->ixType().toIntVect())
                                                  : mfi.tilebox(Efield[1]->ixType().toIntVect(), ng_fieldgather);
        amrex::Box const& tez = (split_pml_field) ? mfi.tilebox(Efield[2]->ixType().toIntVect())
                                                  : mfi.tilebox(Efield[2]->ixType().toIntVect(), ng_fieldgather);

        // loop over cells and update fields
        amrex::ParallelFor(
            tex, nComp_x,
            [=] AMREX_GPU_DEVICE (int i, int j, int k, int n) {
#if (defined WARPX_DIM_XZ) || (defined WARPX_DIM_RZ)
                amrex::ignore_unused(k);
#endif
#if (defined WARPX_DIM_1D_Z)
                amrex::ignore_unused(j,k);
#endif
                amrex::IntVect iv(AMREX_D_DECL(i,j,k));
                const int icomp = 0;
                PEC::SetEfieldOnPEC(icomp, domain_lo, domain_hi, iv, n,
                                           Ex, Ex_nodal, fbndry_lo, fbndry_hi);
            },
            tey, nComp_y,
            [=] AMREX_GPU_DEVICE (int i, int j, int k, int n) {
#if (defined WARPX_DIM_XZ) || (defined WARPX_DIM_RZ)
                amrex::ignore_unused(k);
#endif
#if (defined WARPX_DIM_1D_Z)
                amrex::ignore_unused(j,k);
#endif
                amrex::IntVect iv(AMREX_D_DECL(i,j,k));
                const int icomp = 1;
                PEC::SetEfieldOnPEC(icomp, domain_lo, domain_hi, iv, n,
                                           Ey, Ey_nodal, fbndry_lo, fbndry_hi);
            },
            tez, nComp_z,
            [=] AMREX_GPU_DEVICE (int i, int j, int k, int n) {
#if (defined WARPX_DIM_XZ) || (defined WARPX_DIM_RZ)
                amrex::ignore_unused(k);
#endif
#if (defined WARPX_DIM_1D_Z)
                amrex::ignore_unused(j,k);
#endif
                amrex::IntVect iv(AMREX_D_DECL(i,j,k));
                const int icomp = 2;
                PEC::SetEfieldOnPEC(icomp, domain_lo, domain_hi, iv, n,
                                           Ez, Ez_nodal, fbndry_lo, fbndry_hi);
            }
        );

    }
}


void
PEC::ApplyPECtoBfield (std::array<amrex::MultiFab*, 3> Bfield, const int lev,
                       PatchType patch_type)
{
    auto& warpx = WarpX::GetInstance();
    amrex::Box domain_box = warpx.Geom(lev).Domain();
    if (patch_type == PatchType::coarse) {
        amrex::IntVect ref_ratio = ( (lev > 0) ? WarpX::RefRatio(lev-1) : amrex::IntVect(1) );
        domain_box.coarsen(ref_ratio);
    }
    amrex::IntVect domain_lo = domain_box.smallEnd();
    amrex::IntVect domain_hi = domain_box.bigEnd();
    amrex::GpuArray<int, 3> fbndry_lo;
    amrex::GpuArray<int, 3> fbndry_hi;
    for (int idim=0; idim < AMREX_SPACEDIM; ++idim) {
        fbndry_lo[idim] = WarpX::field_boundary_lo[idim];
        fbndry_hi[idim] = WarpX::field_boundary_hi[idim];
    }
    amrex::IntVect Bx_nodal = Bfield[0]->ixType().toIntVect();
    amrex::IntVect By_nodal = Bfield[1]->ixType().toIntVect();
    amrex::IntVect Bz_nodal = Bfield[2]->ixType().toIntVect();
    amrex::IntVect ng_fieldgather = warpx.get_ng_fieldgather();
    const int nComp_x = Bfield[0]->nComp();
    const int nComp_y = Bfield[1]->nComp();
    const int nComp_z = Bfield[2]->nComp();

#ifdef AMREX_USE_OMP
#pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
#endif
    for (amrex::MFIter mfi(*Bfield[0], amrex::TilingIfNotGPU()); mfi.isValid(); ++mfi) {

        // Extract field data
        amrex::Array4<amrex::Real> const& Bx = Bfield[0]->array(mfi);
        amrex::Array4<amrex::Real> const& By = Bfield[1]->array(mfi);
        amrex::Array4<amrex::Real> const& Bz = Bfield[2]->array(mfi);

        // Extract tileboxes for which to loop
        // For B-field used in Maxwell's update, nodal flag plus cells that particles
        // gather fields from in the guard-cell region are included.
        // Note that for simulations without particles or laser, ng_field_gather is 0
        // and the guard-cell values of the B-field multifab will not be modified.
        amrex::Box const& tbx = mfi.tilebox(Bfield[0]->ixType().toIntVect(), ng_fieldgather);
        amrex::Box const& tby = mfi.tilebox(Bfield[1]->ixType().toIntVect(), ng_fieldgather);
        amrex::Box const& tbz = mfi.tilebox(Bfield[2]->ixType().toIntVect(), ng_fieldgather);

        // loop over cells and update fields
        amrex::ParallelFor(
            tbx, nComp_x,
            [=] AMREX_GPU_DEVICE (int i, int j, int k, int n) {
#if (defined WARPX_DIM_XZ) || (defined WARPX_DIM_RZ)
                amrex::ignore_unused(k);
#endif
#if (defined WARPX_DIM_1D_Z)
                amrex::ignore_unused(j,k);
#endif
                amrex::IntVect iv(AMREX_D_DECL(i,j,k));
                const int icomp = 0;
                PEC::SetBfieldOnPEC(icomp, domain_lo, domain_hi, iv, n,
                                     Bx, Bx_nodal, fbndry_lo, fbndry_hi);
            },
            tby, nComp_y,
            [=] AMREX_GPU_DEVICE (int i, int j, int k, int n) {
#if (defined WARPX_DIM_XZ) || (defined WARPX_DIM_RZ)
                amrex::ignore_unused(k);
#endif
#if (defined WARPX_DIM_1D_Z)
                amrex::ignore_unused(j,k);
#endif
                amrex::IntVect iv(AMREX_D_DECL(i,j,k));
                const int icomp = 1;
                PEC::SetBfieldOnPEC(icomp, domain_lo, domain_hi, iv, n,
                                     By, By_nodal, fbndry_lo, fbndry_hi);
            },
            tbz, nComp_z,
            [=] AMREX_GPU_DEVICE (int i, int j, int k, int n) {
#if (defined WARPX_DIM_XZ) || (defined WARPX_DIM_RZ)
                amrex::ignore_unused(k);
#endif
#if (defined WARPX_DIM_1D_Z)
                amrex::ignore_unused(j,k);
#endif
                amrex::IntVect iv(AMREX_D_DECL(i,j,k));
                const int icomp = 2;
                PEC::SetBfieldOnPEC(icomp, domain_lo, domain_hi, iv, n,
                                     Bz, Bz_nodal, fbndry_lo, fbndry_hi);
            }
        );

    }

}
