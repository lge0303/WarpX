
#include <limits>

#include <ParticleContainer.H>
#include <WarpXParticleContainer.H>
#include <AMReX_AmrParGDB.H>
#include <WarpX_f.H>
#include <WarpX.H>

using namespace amrex;

int WarpXParticleContainer::do_not_push = 0;

#if (BL_SPACEDIM == 2)
void
WarpXParIter::GetPosition (Array<Real>& x, Array<Real>& y, Array<Real>& z) const
{
    amrex::ParIter<0,0,PIdx::nattribs>::GetPosition(x, z);
    y.resize(x.size(), std::numeric_limits<Real>::quiet_NaN());
}

void
WarpXParIter::SetPosition (const Array<Real>& x, const Array<Real>& y, const Array<Real>& z)
{
    amrex::ParIter<0,0,PIdx::nattribs>::SetPosition(x, z);
}
#endif

WarpXParticleContainer::WarpXParticleContainer (AmrCore* amr_core, int ispecies)
    : ParticleContainer<0,0,PIdx::nattribs>(amr_core->GetParGDB())
    , species_id(ispecies)
{
    for (unsigned int i = PIdx::Ex; i < PIdx::nattribs; ++i) {
        communicate_real_comp[i] = false; // Don't need to communicate E and B.
    }
    ReadParameters();
}

void
WarpXParticleContainer::ReadParameters ()
{
    static bool initialized = false;
    if (!initialized)
    {
	ParmParse pp("particles");

        do_tiling = true;  // because the default in amrex is false
	pp.query("do_tiling",  do_tiling);

//        tile_size = IntVect(D_DECL(8,8,8));
        tile_size = IntVect(D_DECL(102400,8,8));

        pp.query("do_not_push", do_not_push);
        
	initialized = true;
    }
}

void
WarpXParticleContainer::AllocData ()
{
    // have to resize here, not in the constructor because grids have not
    // been built when constructor was called.
    reserveData();
    resizeData();
}

void
WarpXParticleContainer::AddOneParticle (int lev, int grid, int tile,
                                        Real x, Real y, Real z,
                                        const std::array<Real,PIdx::nattribs>& attribs)
{
    auto& particle_tile = GetParticles(lev)[std::make_pair(grid,tile)];
    AddOneParticle(particle_tile, x, y, z, attribs); 
}

void
WarpXParticleContainer::AddOneParticle (ParticleTileType& particle_tile,
                                        Real x, Real y, Real z,
                                        const std::array<Real,PIdx::nattribs>& attribs)
{
    ParticleType p;
    p.id()  = ParticleType::NextID();
    p.cpu() = ParallelDescriptor::MyProc();
#if (BL_SPACEDIM == 3)
    p.pos(0) = x;
    p.pos(1) = y;
    p.pos(2) = z;
#elif (BL_SPACEDIM == 2)
    p.pos(0) = x;
    p.pos(1) = z;
#endif
    
    particle_tile.push_back(p);
    particle_tile.push_back_real(attribs);
}

void
WarpXParticleContainer::AddNParticles (int lev,
                                       int n, const Real* x, const Real* y, const Real* z,
				       const Real* vx, const Real* vy, const Real* vz,
				       int nattr, const Real* attr, int uniqueparticles)
{
    BL_ASSERT(nattr == 1);
    const Real* weight = attr;

    int ibegin, iend;
    if (uniqueparticles) {
	ibegin = 0;
	iend = n;
    } else {
	int myproc = ParallelDescriptor::MyProc();
	int nprocs = ParallelDescriptor::NProcs();
	int navg = n/nprocs;
	int nleft = n - navg * nprocs;
	if (myproc < nleft) {
	    ibegin = myproc*(navg+1);
	    iend = ibegin + navg+1;
	} else {
	    ibegin = myproc*navg + nleft;
	    iend = ibegin + navg;
	}
    }

    //  Add to grid 0 and tile 0
    // Redistribute() will move them to proper places.
    std::pair<int,int> key {0,0};
    auto& particle_tile = GetParticles(lev)[key];

    for (int i = ibegin; i < iend; ++i)
    {
        ParticleType p;
        p.id()  = ParticleType::NextID();
        p.cpu() = ParallelDescriptor::MyProc();
#if (BL_SPACEDIM == 3)
        p.pos(0) = x[i];
        p.pos(1) = y[i];
        p.pos(2) = z[i];
#elif (BL_SPACEDIM == 2)
        p.pos(0) = x[i];
        p.pos(1) = z[i];
#endif
        particle_tile.push_back(p);
    }

    std::size_t np = iend-ibegin;

    if (np > 0)
    {
        particle_tile.push_back_real(PIdx::w , weight + ibegin, weight + iend);
        particle_tile.push_back_real(PIdx::ux,     vx + ibegin,     vx + iend);
        particle_tile.push_back_real(PIdx::uy,     vy + ibegin,     vy + iend);
        particle_tile.push_back_real(PIdx::uz,     vz + ibegin,     vz + iend);
        
        for (int comp = PIdx::uz+1; comp < PIdx::nattribs; ++comp)
        {
            particle_tile.push_back_real(comp, np, 0.0);
        }
    }        

    Redistribute();
}

void
WarpXParticleContainer::DepositCharge (Array<std::unique_ptr<MultiFab> >& rho, bool local)
{

    int num_levels = rho.size();
    int finest_level = num_levels - 1;

    // each level deposits it's own particles
    const int ng = rho[0]->nGrow();
    for (int lev = 0; lev < num_levels; ++lev) {       

        rho[lev]->setVal(0.0, ng);

        const auto& gm = m_gdb->Geom(lev);
        const auto& ba = m_gdb->ParticleBoxArray(lev);
        const auto& dm = m_gdb->DistributionMap(lev);
    
        const Real* dx  = gm.CellSize();
        const Real* plo = gm.ProbLo();
        BoxArray nba = ba;
        nba.surroundingNodes();
    
        for (WarpXParIter pti(*this, lev); pti.isValid(); ++pti) {
            const Box& box = nba[pti];
            
            auto& wp = pti.GetAttribs(PIdx::w);
            const auto& particles = pti.GetArrayOfStructs();
            int nstride = particles.dataShape().first;
            const long np  = pti.numParticles();
            
            FArrayBox& rhofab = (*rho[lev])[pti];
            
            WRPX_DEPOSIT_CIC(particles.data(), nstride, np,
                             wp.data(), &this->charge,
                             rhofab.dataPtr(), box.loVect(), box.hiVect(), 
                             plo, dx, &ng);
        }

        if (!local) rho[lev]->SumBoundary(gm.periodicity());
    }

    // now we average down fine to crse
    std::unique_ptr<MultiFab> crse;
    for (int lev = finest_level - 1; lev >= 0; --lev) {
        const BoxArray& fine_BA = rho[lev+1]->boxArray();
        const DistributionMapping& fine_dm = rho[lev+1]->DistributionMap();
        BoxArray coarsened_fine_BA = fine_BA;
        coarsened_fine_BA.coarsen(m_gdb->refRatio(lev));
        
        MultiFab coarsened_fine_data(coarsened_fine_BA, fine_dm, 1, 0);
        coarsened_fine_data.setVal(0.0);
        
        IntVect ratio(D_DECL(2, 2, 2));  // FIXME
        
        for (MFIter mfi(coarsened_fine_data); mfi.isValid(); ++mfi) {
            const Box& bx = mfi.validbox();
            const Box& crse_box = coarsened_fine_data[mfi].box();
            const Box& fine_box = (*rho[lev+1])[mfi].box();
            WRPX_SUM_FINE_TO_CRSE_NODAL(bx.loVect(), bx.hiVect(), ratio.getVect(),
                                        coarsened_fine_data[mfi].dataPtr(), crse_box.loVect(), crse_box.hiVect(),
                                        (*rho[lev+1])[mfi].dataPtr(), fine_box.loVect(), fine_box.hiVect());
        }
        
        rho[lev]->copy(coarsened_fine_data, m_gdb->Geom(lev).periodicity(), FabArrayBase::ADD);
    }
}

std::unique_ptr<MultiFab>
WarpXParticleContainer::GetChargeDensity (int lev, bool local)
{
    const auto& gm = m_gdb->Geom(lev);
    const auto& ba = m_gdb->ParticleBoxArray(lev);
    const auto& dm = m_gdb->DistributionMap(lev);
    BoxArray nba = ba;
    nba.surroundingNodes();

    const std::array<Real,3>& dx = WarpX::CellSize(lev);

    const int ng = WarpX::nox;

    auto rho = std::unique_ptr<MultiFab>(new MultiFab(nba,dm,1,ng));
    rho->setVal(0.0);

    Array<Real> xp, yp, zp;

    for (WarpXParIter pti(*this, lev); pti.isValid(); ++pti)
    {
        const Box& box = pti.validbox();

        auto& wp = pti.GetAttribs(PIdx::w);
            
        const long np  = pti.numParticles();

	// Data on the grid
	FArrayBox& rhofab = (*rho)[pti];

        pti.GetPosition(xp, yp, zp);

#if (BL_SPACEDIM == 3)
	long nx = box.length(0);
	long ny = box.length(1);
	long nz = box.length(2); 
#elif (BL_SPACEDIM == 2)
	long nx = box.length(0);
	long ny = 0;
	long nz = box.length(1); 
#endif

        const std::array<Real,3>& xyzmin = WarpX::LowerCorner(box, lev);

	long nxg = ng;
	long nyg = ng;
	long nzg = ng;
	long lvect = 8;

	warpx_charge_deposition(rhofab.dataPtr(), 
				&np, xp.data(), yp.data(), zp.data(), wp.data(),
				&this->charge, &xyzmin[0], &xyzmin[1], &xyzmin[2], 
				&dx[0], &dx[1], &dx[2], &nx, &ny, &nz,
				&nxg, &nyg, &nzg, &WarpX::nox,&WarpX::noy,&WarpX::noz,
				&lvect, &WarpX::charge_deposition_algo);
				
    }

    if (!local) rho->SumBoundary(gm.periodicity());
    
    return rho;
}

Real WarpXParticleContainer::sumParticleCharge(bool local) {

    const int lev = 0;
    amrex::Real total_charge = 0.0;
    for (WarpXParIter pti(*this, lev); pti.isValid(); ++pti)
    {
        auto& wp = pti.GetAttribs(PIdx::w);
        for (unsigned long i = 0; i < wp.size(); i++) {
            total_charge += wp[i];
        }
    }

    if (!local) ParallelDescriptor::ReduceRealSum(total_charge);
    total_charge *= this->charge;
    return total_charge;
}

Real WarpXParticleContainer::maxParticleVelocity(bool local) {

    const int lev = 0;
    amrex::Real max_v = 0.0;
    for (WarpXParIter pti(*this, lev); pti.isValid(); ++pti)
    {
        auto& ux = pti.GetAttribs(PIdx::ux);
        auto& uy = pti.GetAttribs(PIdx::uy);
        auto& uz = pti.GetAttribs(PIdx::uz);
        for (unsigned long i = 0; i < ux.size(); i++) {
            max_v = std::max(max_v, sqrt(ux[i]*ux[i] + uy[i]*uy[i] + uz[i]*uz[i]));
        }
    }

    if (!local) ParallelDescriptor::ReduceRealMax(max_v);
    return max_v;
}

void
WarpXParticleContainer::PushXES (Real dt)
{
    BL_PROFILE("WPC::PushXES()");

    int num_levels = finestLevel() + 1;

    for (int lev = 0; lev < num_levels; ++lev) {       
        const auto& gm = m_gdb->Geom(lev);
        const RealBox& prob_domain = gm.ProbDomain();
        for (WarpXParIter pti(*this, lev); pti.isValid(); ++pti) {
            auto& particles = pti.GetArrayOfStructs();
            int nstride = particles.dataShape().first;           
            const long np  = pti.numParticles();
            
            auto& attribs = pti.GetAttribs();        
            auto& uxp = attribs[PIdx::ux];
            auto& uyp = attribs[PIdx::uy];
            auto& uzp = attribs[PIdx::uz];
            
            WRPX_PUSH_LEAPFROG_POSITIONS(particles.data(), nstride, np,
                                         uxp.data(), uyp.data(), uzp.data(), &dt,
                                         prob_domain.lo(), prob_domain.hi());
        }
    }
}

void
WarpXParticleContainer::PushX (Real dt)
{
    for (int lev = 0; lev <= finestLevel(); ++lev) {
        PushX(lev, dt);
    }
}

void
WarpXParticleContainer::PushX (int lev, Real dt)
{
    BL_PROFILE("WPC::PushX()");
    BL_PROFILE_VAR_NS("WPC::PushX::Copy", blp_copy);
    BL_PROFILE_VAR_NS("WPC:PushX::Push", blp_pxr_pp);

    if (do_not_push) return;

    Array<Real> xp, yp, zp, giv;

    for (WarpXParIter pti(*this, lev); pti.isValid(); ++pti)
    {

        auto& attribs = pti.GetAttribs();

        auto& uxp = attribs[PIdx::ux];
        auto& uyp = attribs[PIdx::uy];
        auto& uzp = attribs[PIdx::uz];
        
        const long np = pti.numParticles();

        giv.resize(np);

        //
        // copy data from particle container to temp arrays
        //
        BL_PROFILE_VAR_START(blp_copy);
        pti.GetPosition(xp, yp, zp);
        BL_PROFILE_VAR_STOP(blp_copy);

        //
        // Particle Push
        //
        BL_PROFILE_VAR_START(blp_pxr_pp);
        warpx_particle_pusher_positions(&np, xp.data(), yp.data(), zp.data(),
                                        uxp.data(), uyp.data(), uzp.data(), giv.data(), &dt);
        BL_PROFILE_VAR_STOP(blp_pxr_pp);

        //
        // copy particle data back
        //
        BL_PROFILE_VAR_START(blp_copy);
        pti.SetPosition(xp, yp, zp);
        BL_PROFILE_VAR_STOP(blp_copy);
    }
}

