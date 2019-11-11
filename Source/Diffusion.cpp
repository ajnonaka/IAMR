//fixme, for writesingle level plotfile
#include<AMReX_PlotFileUtil.H>
//

#include <AMReX_ParmParse.H>

#include <Diffusion.H>
#include <NavierStokesBase.H>

//fixme -- remove once MLTensorOp working
#include <AMReX_MultiGrid.H>
#include <AMReX_CGSolver.H>
#include <AMReX_MLNodeLaplacian.H>
#include <fstream>
//

#include <DIFFUSION_F.H>

#include <algorithm>
#include <cfloat>
#include <iomanip>
#include <array>

#include <iostream>

#include <AMReX_MLMG.H>
#ifdef AMREX_USE_EB
#include <AMReX_EBFArrayBox.H>
#include <AMReX_MLEBABecLap.H>
#include <AMReX_MLEBTensorOp.H>
#include <AMReX_EBMultiFabUtil.H>
#include <AMReX_EBFabFactory.H>
#else
#include <AMReX_MLABecLaplacian.H>
#include <AMReX_MLTensorOp.H>
#endif

using namespace amrex;

#if defined(BL_OSF1)
#if defined(BL_USE_DOUBLE)
const Real BL_BOGUS      = DBL_QNAN;
#else
const Real BL_BOGUS      = FLT_QNAN;
#endif
#else
const Real BL_BOGUS      = 1.e200;
#endif

const Real BL_SAFE_BOGUS = -666.e200;

#define DEF_LIMITS(fab,fabdat,fablo,fabhi)   \
const int* fablo = (fab).loVect();           \
const int* fabhi = (fab).hiVect();           \
Real* fabdat = (fab).dataPtr();

#define DEF_CLIMITS(fab,fabdat,fablo,fabhi)  \
const int* fablo = (fab).loVect();           \
const int* fabhi = (fab).hiVect();           \
const Real* fabdat = (fab).dataPtr();

namespace
{
    bool initialized = false;
    static int agglomeration = 1;
    static int consolidation = 1;
    static int max_fmg_iter = 0;
    static int use_hypre = 0;
    static int hypre_verbose = 0;
}
//
// Set default values in !initialized section of code in constructor!!!
//
int         Diffusion::verbose;
Real        Diffusion::visc_tol;
int         Diffusion::do_reflux;
int         Diffusion::max_order;
int         Diffusion::scale_abec;
int         Diffusion::use_cg_solve;
int         Diffusion::tensor_max_order;
int         Diffusion::use_tensor_cg_solve;
bool        Diffusion::use_mg_precond_flag;

Vector<Real> Diffusion::visc_coef;
Vector<int>  Diffusion::is_diffusive;

void
Diffusion::Finalize ()
{
    visc_coef.clear();
    is_diffusive.clear();

    initialized = false;
}

Diffusion::Diffusion (Amr*               Parent,
                      NavierStokesBase*  Caller,
                      Diffusion*         Coarser,
                      int                num_state,
                      FluxRegister*      Viscflux_reg,
                      const Vector<int>&  _is_diffusive,
                      const Vector<Real>& _visc_coef)
    :
    parent(Parent),
    navier_stokes(Caller),
    grids(navier_stokes->boxArray()),
    dmap(navier_stokes->DistributionMap()),
    level(navier_stokes->Level()),
    coarser(Coarser),
    finer(0),
    NUM_STATE(num_state),
    viscflux_reg(Viscflux_reg)

{
    if (!initialized)
    {
        //
        // Set defaults here!!!
        //
        Diffusion::verbose             = 0;
        Diffusion::visc_tol            = 1.0e-10;
        Diffusion::do_reflux           = 1;
        Diffusion::max_order           = 2;
        Diffusion::scale_abec          = 0;
        Diffusion::use_cg_solve        = 0;
        Diffusion::tensor_max_order    = 2;
        Diffusion::use_tensor_cg_solve = 0;
        Diffusion::use_mg_precond_flag = false;

        int use_mg_precond = 0;

        ParmParse ppdiff("diffuse");

        ppdiff.query("v",                   verbose);
        ppdiff.query("max_order",           max_order);
        ppdiff.query("scale_abec",          scale_abec);
        ppdiff.query("use_cg_solve",        use_cg_solve);
        ppdiff.query("use_mg_precond",      use_mg_precond);
        ppdiff.query("tensor_max_order",    tensor_max_order);
        ppdiff.query("use_tensor_cg_solve", use_tensor_cg_solve);

        ppdiff.query("agglomeration", agglomeration);
        ppdiff.query("consolidation", consolidation);
        ppdiff.query("max_fmg_iter", max_fmg_iter);
#ifdef AMREX_USE_HYPRE
        ppdiff.query("use_hypre", use_hypre);
        ppdiff.query("hypre_verbose", hypre_verbose);
#endif

        use_mg_precond_flag = (use_mg_precond ? true : false);

        ParmParse pp("ns");

        pp.query("visc_tol",  visc_tol);
        pp.query("do_reflux", do_reflux);

        do_reflux = (do_reflux ? 1 : 0);

        const int n_visc = _visc_coef.size();
        const int n_diff = _is_diffusive.size();

        if (n_diff < NUM_STATE)
            amrex::Abort("Diffusion::Diffusion(): is_diffusive array is not long enough");

        if (n_visc < NUM_STATE)
            amrex::Abort("Diffusion::Diffusion(): visc_coef array is not long enough");

        if (n_visc > NUM_STATE)
            amrex::Abort("Diffusion::Diffusion(): TOO MANY diffusion coeffs were given!");

        visc_coef.resize(NUM_STATE);
        is_diffusive.resize(NUM_STATE);

        for (int i = 0; i < NUM_STATE; i++)
        {
            is_diffusive[i] = _is_diffusive[i];
            visc_coef[i] = _visc_coef[i];
        }

        echo_settings();

        amrex::ExecOnFinalize(Diffusion::Finalize);

        initialized = true;
    }

    if (level > 0)
    {
        crse_ratio = parent->refRatio(level-1);
        coarser->finer = this;
    }
}

Diffusion::~Diffusion () {}

FluxRegister*
Diffusion::viscFluxReg ()
{
    return viscflux_reg;
}

int
Diffusion::maxOrder() const
{
    return max_order;
}

int
Diffusion::tensorMaxOrder() const
{
    return tensor_max_order;
}

void
Diffusion::echo_settings () const
{
    //
    // Print out my settings.
    //
  if (verbose)
    {
        amrex::Print() << "Diffusion settings...\n";
        amrex::Print() << "  From diffuse:\n";
        amrex::Print() << "   use_cg_solve        = " << use_cg_solve        << '\n';
        amrex::Print() << "   use_tensor_cg_solve = " << use_tensor_cg_solve << '\n';
        amrex::Print() << "   use_mg_precond_flag = " << use_mg_precond_flag << '\n';
        amrex::Print() << "   max_order           = " << max_order           << '\n';
        amrex::Print() << "   tensor_max_order    = " << tensor_max_order    << '\n';
        amrex::Print() << "   scale_abec          = " << scale_abec          << '\n';
    
        amrex::Print() << "\n\n  From ns:\n";
        amrex::Print() << "   do_reflux           = " << do_reflux << '\n';
        amrex::Print() << "   visc_tol            = " << visc_tol  << '\n';
    
        amrex::Print() << "   is_diffusive =";
        for (int i =0; i < NUM_STATE; i++)
            amrex::Print() << "  " << is_diffusive[i];
    
        amrex::Print() << "\n   visc_coef =";
        for (int i = 0; i < NUM_STATE; i++)
            amrex::Print() << "  " << visc_coef[i];

        amrex::Print() << '\n';
    }
}

Real
Diffusion::get_scaled_abs_tol (const MultiFab& rhs,
                               Real            reduction) //const
{
    return reduction * rhs.norm0();
}


// towards simplification of this, why pass all of geom, vol, area, add_hoop_stress
// when geom can be used to get all the others?
void
Diffusion::diffuse_scalar (const Vector<MultiFab*>&  S_old,
                           const Vector<MultiFab*>&  Rho_old,
                           Vector<MultiFab*>&        S_new,
                           const Vector<MultiFab*>&  Rho_new,
                           int                       S_comp,
                           int                       num_comp,
                           int                       Rho_comp,
                           Real                      prev_time,
                           Real                      curr_time,
                           Real                      be_cn_theta,
                           const MultiFab&           rho_half,
                           int                       rho_flag,
                           MultiFab* const*          fluxn,
                           MultiFab* const*          fluxnp1,
                           int                       fluxComp,
                           MultiFab*                 delta_rhs, 
                           int                       rhsComp,
                           const MultiFab*           alpha_in, 
                           int                       alpha_in_comp,
                           const MultiFab* const*    betan, 
                           const MultiFab* const*    betanp1,
                           int                       betaComp,
                           const Vector<Real>&       visc_coef,
                           int                       visc_coef_comp,
                           const MultiFab&           volume,
                           const MultiFab* const*    area,
                           const IntVect&            cratio,
                           const BCRec&              bc,
                           const Geometry&           geom,
                           bool                      add_hoop_stress,
                           const SolveMode&          solve_mode,
                           bool                      add_old_time_divFlux,
                           const amrex::Vector<int>& is_diffusive)
{
    //
    // This routine expects that physical BC's have been loaded into
    // the grow cells of the old and new state at this level.  If rho_flag==2,
    // the values there are rho.phi, where phi is the quantity being diffused.
    // Values in these cells will be preserved.  Also, if there are any
    // explicit update terms, these have already incremented the new state
    // on the valid region (i.e., on the valid region the new state is the old
    // state + dt*Div(explicit_fluxes), e.g.)
    //
    
    if (verbose)
      amrex::Print() << "... Diffusion::diffuse_scalar(): \n" 
		     << " lev: " << level << '\n';

#if (BL_SPACEDIM == 3)
    // Here we ensure that R-Z related routines cannot be called in 3D
    if (add_hoop_stress){
      amrex::Abort("in diffuse_scalar: add_hoop_stress for R-Z geometry called in 3D !");
    }
#endif

#ifdef AMREX_USE_EB
  // Here we ensure that R-Z cannot work with EB (for now)
    if (add_hoop_stress){
      amrex::Abort("in diffuse_scalar: add_hoop_stress for R-Z geometry not yet working with EB support !");
    }
#endif

    //for now, remove RZ until the Vol scaling issue is fixed...
    // want
    //   volume scaling for non-EB RZ
    //   error for EB-RZ, "under development"
    //   remove volume scaling for everything else
    if (add_hoop_stress)
      amrex::Abort("Diffusion::diffuse_scalar(): R-Z geometry under development!");
    

    // FIXME -- nned to check on ghost cells of all MFs passed in
    //
    //FIXME - check that parameters betan betanp1, alpha are EB aware
    // only acoeff and bcoeff need to be EB aware; they're what goes to MLMG
    // what about fluxes?
    //
    
    bool has_coarse_data = S_new.size() > 1;

    const Real strt_time = ParallelDescriptor::second();

    int allnull, allthere;
    checkBeta(betan, allthere, allnull);
    checkBeta(betanp1, allthere, allnull);

    //
    // At this point, S_old has bndry at time N, S_new has bndry at time N+1
    //
    // CEG - looking back through code suggests this is not true for IAMR
    //  right now. Would need to fix in calling functions...
    //MultiFab& S_old = navier_stokes->get_old_data(State_Type);
    //MultiFab& S_new = navier_stokes->get_new_data(State_Type);

    // Talking with weiqun, thinks no ghost cells are actually needed for MLMG, only
    // Note for cell-centered solver, you need to cal setLevelBC.  That needs to have
    // one ghost cell if there is Dirichlet BC.
    // Trying out ng = 0 ... get failed assertion from setLevelBC bc Soln is used a
    //   temporary for that
    // const int ng = eb_ngrow;
    // const int ng_rhs = eb_ngrow;
    //const int ng = 1;
    //const int ng_rhs = 0;

    Real dt = curr_time - prev_time;
    const int ng = 1;
    // FIXME? going with the idea that S_old could be null (mac sync does this)
    BL_ASSERT(S_new[0]->nGrow()>0); // && S_old[0]->nGrow()>0);
    const BoxArray& ba = S_new[0]->boxArray();
    const DistributionMapping& dm = S_new[0]->DistributionMap();
    const DistributionMapping* dmc = (has_coarse_data ? &(S_new[1]->DistributionMap()) : 0);
    const BoxArray* bac = (has_coarse_data ? &(S_new[1]->boxArray()) : 0);
  
    BL_ASSERT(solve_mode==ONEPASS || (delta_rhs && delta_rhs->boxArray()==ba));
    BL_ASSERT(volume.DistributionMap() == dm);
    
    const auto& ebfactory = S_new[0]->Factory();
    
    MultiFab Rhs(ba,dm,1,0,MFInfo(),ebfactory);
    MultiFab Soln(ba,dm,1,ng,MFInfo(),ebfactory);
    MultiFab alpha(ba,dm,1,0,MFInfo(),ebfactory);
    
    std::array<MultiFab,AMREX_SPACEDIM> bcoeffs;
    for (int n = 0; n < BL_SPACEDIM; n++)
    {
      BL_ASSERT(area[n]->DistributionMap() == dm); 
      bcoeffs[n].define(area[n]->boxArray(),dm,1,0,MFInfo(),ebfactory);
    }
    auto Solnc = std::unique_ptr<MultiFab>(new MultiFab());
    if (has_coarse_data)
    {
      Solnc->define(*bac, *dmc, 1, ng, MFInfo(), S_new[1]->Factory());
    }

    std::array<LinOpBCType,AMREX_SPACEDIM> mlmg_lobc;
    std::array<LinOpBCType,AMREX_SPACEDIM> mlmg_hibc;

    // why bother making this time n operator for purely implicit cases?
    LPInfo infon;
    infon.setAgglomeration(agglomeration);
    infon.setConsolidation(consolidation);
    infon.setMetricTerm(false);
    infon.setMaxCoarseningLevel(0);
  
#ifdef AMREX_USE_EB
    // create the right data holder for passing to MLEBABecLap
    const auto& ebf = &(dynamic_cast<EBFArrayBoxFactory const&>(ebfactory));
    
    MLEBABecLap opn({geom}, {ba}, {dm}, infon, {ebf});
    std::array<const amrex::MultiCutFab*,AMREX_SPACEDIM>areafrac = ebf->getAreaFrac();
#else	  
    MLABecLaplacian opn({geom}, {ba}, {dm}, infon);
#endif  
   
    opn.setMaxOrder(max_order);
    MLMG mgn(opn);
    mgn.setVerbose(verbose);

    LPInfo infonp1;
    infonp1.setAgglomeration(agglomeration);
    infonp1.setConsolidation(consolidation);
    infonp1.setMetricTerm(false);
      
#ifdef AMREX_USE_EB
    MLEBABecLap opnp1({geom}, {ba}, {dm}, infonp1, {ebf});
#else	  
    MLABecLaplacian opnp1({geom}, {ba}, {dm}, infonp1);
#endif
  
    opnp1.setMaxOrder(max_order);
    
    MLMG mgnp1(opnp1);
    if (use_hypre)
    {
      mgnp1.setBottomSolver(MLMG::BottomSolver::hypre);
      mgnp1.setBottomVerbose(hypre_verbose);
    }
    mgnp1.setMaxFmgIter(max_fmg_iter);
    mgnp1.setVerbose(verbose);

    setDomainBC(mlmg_lobc, mlmg_hibc, bc); // Same for all comps, by assumption
    // FIXME -- need to check on DefaultGeometry().getPeriodicity() in setDomainBC_msd()
    opn.setDomainBC(mlmg_lobc, mlmg_hibc);
    opnp1.setDomainBC(mlmg_lobc, mlmg_hibc);
 
    for (int icomp=0; icomp<num_comp; ++icomp)
    {
      if (verbose)
      {
	amrex::Print() << "diffusing scalar "<<icomp+1<<" of "<<num_comp << "\n";
	amrex::Print() << "rho flag "<<rho_flag << "\n";
      }

      int sigma = S_comp + icomp;

      if (is_diffusive[icomp] == 0)
      {
	for (int n = 0; n < BL_SPACEDIM; n++)
	{
	  if (fluxn[n]!=0 && fluxnp1[n]!=0)
          { 
	    fluxn[n]->setVal(0,fluxComp+icomp,1);
	    fluxnp1[n]->setVal(0,fluxComp+icomp,1);
	  }
	}
	break;
      }

      if (add_old_time_divFlux && be_cn_theta!=1)
      {
	Real a = 0.0;
	Real b = -(1.0-be_cn_theta)*dt;
	if (allnull)
	  b *= visc_coef[visc_coef_comp + icomp];

	if(verbose)
	  Print()<<"Adding old time diff ...\n";
        
	{
	  if (has_coarse_data)
	  {
	    MultiFab::Copy(*Solnc,*S_old[1],sigma,0,1,ng);
	    // fixme? need to address other rho_flags too?
	    if (rho_flag == 2)
	    {
	      MultiFab::Divide(*Solnc,*Rho_old[1],Rho_comp,0,1,ng);
	    }
	    opn.setCoarseFineBC(Solnc.get(), cratio[0]);
	  }
	  MultiFab::Copy(Soln,*S_old[0],sigma,0,1,ng);
	  if (rho_flag == 2)
	  {
	    MultiFab::Divide(Soln,*Rho_old[0],Rho_comp,0,1,ng);
	  }
	  opn.setLevelBC(0, &Soln);
	}

	{
	  opn.setScalars(a,b);
	  // not needed bc a=0
	  //opn.setACoeffs(0, alpha);
	}

	{
	  computeBeta(bcoeffs, betan, betaComp+icomp, geom, area, add_hoop_stress);
	  opn.setBCoeffs(0, amrex::GetArrOfConstPtrs(bcoeffs));
	}
      
	mgn.apply({&Rhs},{&Soln});

	AMREX_D_TERM(MultiFab flxx(*fluxn[0], amrex::make_alias, fluxComp+icomp, 1);,
		     MultiFab flxy(*fluxn[1], amrex::make_alias, fluxComp+icomp, 1);,
		     MultiFab flxz(*fluxn[2], amrex::make_alias, fluxComp+icomp, 1););
	std::array<MultiFab*,AMREX_SPACEDIM> fp{AMREX_D_DECL(&flxx,&flxy,&flxz)};
	mgn.getFluxes({fp},{&Soln});
  
	int nghost = 0;
#ifdef AMREX_USE_EB
#ifdef _OPENMP
#pragma omp parallel
#endif
	for (MFIter mfi(Soln,true); mfi.isValid(); ++mfi)
	{  
	  Box bx = mfi.tilebox();
	  
	  // need face-centered tilebox for each direction
	  D_TERM(const Box& xbx = mfi.tilebox(IntVect::TheDimensionVector(0));,
		 const Box& ybx = mfi.tilebox(IntVect::TheDimensionVector(1));,
		 const Box& zbx = mfi.tilebox(IntVect::TheDimensionVector(2)););
	  
	  // this is to check efficiently if this tile contains any eb stuff
	  const EBFArrayBox& in_fab = static_cast<EBFArrayBox const&>(Soln[mfi]);
	  const EBCellFlagFab& flags = in_fab.getEBCellFlagFab();
	  
	  if(flags.getType(amrex::grow(bx, nghost)) == FabType::covered)
	  {
	    // If tile is completely covered by EB geometry, set 
	    // value to some very large number so we know if
	    // we accidentaly use these covered vals later in calculations
	    D_TERM(fluxn[0]->setVal(1.2345e30, xbx, fluxComp+icomp, 1);,
		   fluxn[1]->setVal(1.2345e30, ybx, fluxComp+icomp, 1);,
		   fluxn[2]->setVal(1.2345e30, zbx, fluxComp+icomp, 1););
	  }
	  else
	  {
	    // No cut cells in tile + nghost-cell witdh halo -> use non-eb routine
	    if(flags.getType(amrex::grow(bx, nghost)) == FabType::regular)
	    {		
	      for (int i = 0; i < BL_SPACEDIM; ++i)
	      {
		(*fluxn[i])[mfi].mult(-b/dt,fluxComp+icomp,1);
		(*fluxn[i])[mfi].mult((*area[i])[mfi],0,fluxComp+icomp,1);
	      }
	    }
	    else
	    {
	      // Use EB routines
	      for (int i = 0; i < BL_SPACEDIM; ++i)
	      {
		(*fluxn[i])[mfi].mult(-b/dt,fluxComp+icomp,1);
		(*fluxn[i])[mfi].mult((*area[i])[mfi],0,fluxComp+icomp,1);
		(*fluxn[i])[mfi].mult((*areafrac[i])[mfi],0,fluxComp+icomp,1);
	      }
	    }  
	  }        
	}
#else // non-EB
	for (int i = 0; i < BL_SPACEDIM; ++i)
	{
	  // Here we keep the weighting by the volume for non-EB && R-Z case
	  // The flag has already been checked for only 2D at the begining of the routine
	  if (add_hoop_stress)
	  {    
	    (*fluxn[i]).mult(-b/(dt * geom.CellSize()[i]),fluxComp+icomp,1,0);
	  }
	  else // Generic case for non-EB and 2D or 3D Cartesian
	  {
	    MultiFab::Multiply(*fluxn[i],(*area[i]),0,fluxComp+icomp,1,nghost);
	    (*fluxn[i]).mult(-b/dt,fluxComp+icomp,1,nghost);
	  }
	}
#endif
      }
      else
      {
	for (int n = 0; n < BL_SPACEDIM; n++)
	{
	  fluxn[n]->setVal(0,fluxComp+icomp,1);
	}
	Rhs.setVal(0);
      }

      //
      // If this is a predictor step, put "explicit" updates passed via S_new
      // into Rhs after scaling by rho_half if reqd, so they dont get lost,
      // pull it off S_new to avoid double counting
      //   (for rho_flag == 1:
      //       S_new = S_old - dt.(U.Grad(phi)); want Rhs -= rho_half.(U.Grad(phi)),
      //    else
      //       S_new = S_old - dt.Div(U.Phi),   want Rhs -= Div(U.Phi) )
      //
      if (solve_mode == PREDICTOR)
      {
#ifdef _OPENMP
#pragma omp parallel
#endif
      {
        FArrayBox tmpfab;
        for (MFIter Smfi(*S_new[0], true); Smfi.isValid(); ++Smfi)
        {
            const Box& box = Smfi.tilebox();
            tmpfab.resize(box,1);
            tmpfab.copy((*S_new[0])[Smfi],box,sigma,box,0,1);
            tmpfab.minus((*S_old[0])[Smfi],box,sigma,0,1);
            (*S_new[0])[Smfi].minus(tmpfab,box,0,sigma,1); // Remove this term from S_new
            tmpfab.mult(1.0/dt,box,0,1);
            if (rho_flag == 1)
              tmpfab.mult(rho_half[Smfi],box,0,0,1);
            if (alpha_in!=0)
              tmpfab.mult((*alpha_in)[Smfi],box,alpha_in_comp+icomp,0,1);            
            Rhs[Smfi].plus(tmpfab,box,0,rhsComp+icomp,1);
	}
      }
      }

      //
      // Add body sources (like chemistry contribution)
      //
      if (delta_rhs != 0)
      {
#ifdef _OPENMP
#pragma omp parallel
#endif
      {
        FArrayBox tmpfab;
        for (MFIter mfi(*delta_rhs,true); mfi.isValid(); ++mfi)
        {
            const Box& box = mfi.tilebox();
            tmpfab.resize(box,1);
            tmpfab.copy((*delta_rhs)[mfi],box,rhsComp+icomp,box,0,1);          
            tmpfab.mult(dt,box,0,1);
#if (BL_SPACEDIM == 2)
	    // Here we keep the weighting by the volume for non-EB && R-Z case 
            if (add_hoop_stress){
              tmpfab.mult(volume[mfi],box,0,0,1);
            }
#endif
            Rhs[mfi].plus(tmpfab,box,0,0,1);

            if (rho_flag == 1)
              Rhs[mfi].mult(rho_half[mfi],box,0,0);
	}
      }
      }

      //
      // Add hoop stress for x-velocity in r-z coordinates
      // Note: we have to add hoop stress explicitly because the hoop
      // stress which is added through the operator in getViscOp
      // is eliminated by setting a = 0.
      //
#if (BL_SPACEDIM == 2) 
      if (add_hoop_stress)
      {
	if (verbose) Print() << "Doing RZ coord..." << std::endl;

#ifdef _OPENMP
#pragma omp parallel
#endif
	{
	  Vector<Real> rcen;

	  for (MFIter Rhsmfi(Rhs,true); Rhsmfi.isValid(); ++Rhsmfi)
	  {
	    const Box& bx   = Rhsmfi.tilebox();
	    const Box& rbx  = Rhs[Rhsmfi].box();
	    const Box& sbx  = (*S_old[0])[Rhsmfi].box();
	    const Box& vbox = volume[Rhsmfi].box();
	    
	    rcen.resize(bx.length(0));
	    geom.GetCellLoc(rcen, bx, 0);
	    
	    const int*  lo      = bx.loVect();
	    const int*  hi      = bx.hiVect();
	    const int*  rlo     = rbx.loVect();
	    const int*  rhi     = rbx.hiVect();
	    const int*  slo     = sbx.loVect();
	    const int*  shi     = sbx.hiVect();
	    Real*       rhs     = Rhs[Rhsmfi].dataPtr();
	    const Real* sdat    = (*S_old[0])[Rhsmfi].dataPtr(sigma);
	    const Real* rcendat = rcen.dataPtr();
	    const Real  coeff   = (1.0-be_cn_theta)*visc_coef[visc_coef_comp+icomp]*dt;
	    const Real* voli    = volume[Rhsmfi].dataPtr();
	    const int*  vlo     = vbox.loVect();
	    const int*  vhi     = vbox.hiVect();
	    
	    hooprhs(ARLIM(lo),ARLIM(hi),
		    rhs, ARLIM(rlo), ARLIM(rhi), 
		    sdat, ARLIM(slo), ARLIM(shi),
		    rcendat, &coeff, voli, ARLIM(vlo),ARLIM(vhi));
	  }
	}
      }
#endif
       
      //
      // Increment Rhs with S_old*V (or S_old*V*rho_half if rho_flag==1
      //                             or S_old*V*rho_old  if rho_flag==3)
      //  (Note: here S_new holds S_old, but also maybe an explicit increment
      //         from advection if solve_mode != PREDICTOR)
      //
      MultiFab::Copy(Soln,*S_new[0],sigma,0,1,0);
#ifdef _OPENMP
#pragma omp parallel
#endif

      for (MFIter mfi(Soln,true); mfi.isValid(); ++mfi)
      {
	const Box& box = mfi.tilebox();
#if (BL_SPACEDIM == 2)
	// Here we keep the weighting by the volume for non-EB && R-Z case 
	if (add_hoop_stress)
	{    
	  Soln[mfi].mult(volume[mfi],box,0,0,1);
	}
#endif
	if (rho_flag == 1)
	  Soln[mfi].mult(rho_half[mfi],box,0,0,1);
	if (rho_flag == 3)
	  Soln[mfi].mult((*Rho_old[0])[mfi],box,Rho_comp,0,1);
	if (alpha_in!=0)
	  Soln[mfi].mult((*alpha_in)[mfi],box,alpha_in_comp+icomp,0,1);
	Rhs[mfi].plus(Soln[mfi],box,0,0,1);
      }

      //Fixme???
      // dev fillPatch'es S_new (both levels) before passing to MLMG op
      // It's now assumed that S_new has been FillPatch'ed before passing to diffuse_scalar
      // CHECK THAT THIS IS ACTUALLY DONE...

      //
      // Construct viscous operator with bndry data at time N+1.
      //
      Real a = 1.0;
      Real b = be_cn_theta*dt;
      if (allnull)
      {
	b *= visc_coef[visc_coef_comp+icomp];
      }

      Real rhsscale = 1.0;
      
      {
	if (has_coarse_data)
	{
	  MultiFab::Copy(*Solnc,*S_new[1],sigma,0,1,ng);
	  if (rho_flag == 2)
	  {
	    MultiFab::Divide(*Solnc,*Rho_new[1],Rho_comp,0,1,ng);
	  }
	  // what about rho_flag ==3 ?
	  opnp1.setCoarseFineBC(Solnc.get(), cratio[0]);
	}
	
	MultiFab::Copy(Soln,*S_new[0],sigma,0,1,ng);
	if (rho_flag == 2)
	{
	  MultiFab::Divide(Soln,*Rho_new[0],Rho_comp,0,1,ng);
	}
	//EB_set_covered(Soln, 0, AMREX_SPACEDIM, ng, 1.);
	opnp1.setLevelBC(0, &Soln);
      }

      {
	std::pair<Real,Real> scalars;
	
	computeAlpha(alpha, scalars, a, b, rho_half, rho_flag,
		     &rhsscale, alpha_in, alpha_in_comp+icomp,
		     Rho_new[0], Rho_comp,
		     geom, volume, add_hoop_stress);
	opnp1.setScalars(scalars.first, scalars.second);
	opnp1.setACoeffs(0, alpha);
      }
 
      {
	computeBeta(bcoeffs, betanp1, betaComp+icomp, geom, area, add_hoop_stress);
	opnp1.setBCoeffs(0, amrex::GetArrOfConstPtrs(bcoeffs));
      }
      // rhsscale =1. above
      //Rhs.mult(rhsscale,0,1);
      const Real S_tol     = visc_tol;
      const Real S_tol_abs = get_scaled_abs_tol(Rhs, visc_tol);

      mgnp1.solve({&Soln}, {&Rhs}, S_tol, S_tol_abs);
     
      AMREX_D_TERM(MultiFab flxx(*fluxnp1[0], amrex::make_alias, fluxComp+icomp, 1);,
		   MultiFab flxy(*fluxnp1[1], amrex::make_alias, fluxComp+icomp, 1);,
		   MultiFab flxz(*fluxnp1[2], amrex::make_alias, fluxComp+icomp, 1););
      std::array<MultiFab*,AMREX_SPACEDIM> fp{AMREX_D_DECL(&flxx,&flxy,&flxz)};
      mgnp1.getFluxes({fp});
       
      int nghost = fluxnp1[0]->nGrow(); // this = 0
      
#ifdef AMREX_USE_EB
#ifdef _OPENMP
#pragma omp parallel
#endif
      for (MFIter mfi(Soln,true); mfi.isValid(); ++mfi)
      {  
	Box bx = mfi.tilebox();
	
	// need face-centered tilebox for each direction
	D_TERM(const Box& xbx = mfi.tilebox(IntVect::TheDimensionVector(0));,
	       const Box& ybx = mfi.tilebox(IntVect::TheDimensionVector(1));,
	       const Box& zbx = mfi.tilebox(IntVect::TheDimensionVector(2)););

	std::array<const Box*,AMREX_SPACEDIM> nbx{AMREX_D_DECL(&xbx,&ybx,&zbx)};
	// this is to check efficiently if this tile contains any eb stuff
	const EBFArrayBox& in_fab = static_cast<EBFArrayBox const&>(Soln[mfi]);
	const EBCellFlagFab& flags = in_fab.getEBCellFlagFab();
      
      if(flags.getType(amrex::grow(bx, nghost)) == FabType::covered)
      {
	// If tile is completely covered by EB geometry, set 
	// value to some very large number so we know if
	// we accidentaly use these covered vals later in calculations
	D_TERM(fluxnp1[0]->setVal(1.2345e30, xbx, fluxComp+icomp, 1);,
	       fluxnp1[1]->setVal(1.2345e30, ybx, fluxComp+icomp, 1);,
	       fluxnp1[2]->setVal(1.2345e30, zbx, fluxComp+icomp, 1););
      }
      else
      {
	// No cut cells in tile + nghost-cell witdh halo -> use non-eb routine
	if(flags.getType(amrex::grow(bx, nghost)) == FabType::regular)
        {
	  for (int i = 0; i < BL_SPACEDIM; ++i)
	  {
	    (*fluxnp1[i])[mfi].mult(b/dt,*nbx[i],fluxComp+icomp,1);
	    (*fluxnp1[i])[mfi].mult((*area[i])[mfi],*nbx[i],0,fluxComp+icomp,1);
	  }
	}
        else
        {
        // Use EB routines
          for (int i = 0; i < BL_SPACEDIM; ++i)
          {
            (*fluxnp1[i])[mfi].mult(b/dt,*nbx[i],fluxComp+icomp,1);
            (*fluxnp1[i])[mfi].mult((*area[i])[mfi],*nbx[i],0,fluxComp+icomp,1);
            (*fluxnp1[i])[mfi].mult((*areafrac[i])[mfi],*nbx[i],0,fluxComp+icomp,1);
          }
        }
      }        
    }
#else
    // Non-EB here
    for (int i = 0; i < BL_SPACEDIM; ++i)
    {
      // Here we keep the weighting by the volume for non-EB && R-Z case
      // The flag has already been checked for only 2D at the begining of the routine
      if (add_hoop_stress)
      {    
        (*fluxnp1[i]).mult(b/(dt * geom.CellSize()[i]),fluxComp+icomp,1,0);
      }
      else // Generic case for non-EB and 2D or 3D Cartesian
      {
        MultiFab::Multiply(*fluxnp1[i],(*area[i]),0,fluxComp+icomp,1,nghost);
	      (*fluxnp1[i]).mult(b/dt,fluxComp+icomp,1,nghost);
      }      
    }
#endif

     //
     // Copy into state variable at new time, without bc's
     //
     MultiFab::Copy(*S_new[0],Soln,0,sigma,1,0);

     if (rho_flag == 2) {
#ifdef _OPENMP
#pragma omp parallel
#endif
        for (MFIter Smfi(*S_new[0],true); Smfi.isValid(); ++Smfi) {
                (*S_new[0])[Smfi].mult((*Rho_new[0])[Smfi],Smfi.tilebox(),Rho_comp,sigma,1);
        }
     }

     if (verbose) amrex::Print() << "Done with diffuse_scalar" << "\n";
     
    }

    if (verbose)
    {
      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
      Real      run_time = ParallelDescriptor::second() - strt_time;
      ParallelDescriptor::ReduceRealMax(run_time,IOProc);
      amrex::Print() << "Diffusion::diffuse_scalar() time: " << run_time << '\n';
    }
}

void
Diffusion::diffuse_velocity (Real                   dt,
                             Real                   be_cn_theta,
                             const MultiFab&        rho_half,
                             int                    rho_flag,
                             MultiFab*              delta_rhs,
                             const MultiFab* const* betan, 
                             const MultiFab* const* betanp1)
{
    diffuse_velocity(dt, be_cn_theta, rho_half, rho_flag,
                     delta_rhs, 0, betan, betanp1, 0);
}

void
Diffusion::diffuse_velocity (Real                   dt,
                             Real                   be_cn_theta,
                             const MultiFab&        rho_half,
                             int                    rho_flag,
                             MultiFab*              delta_rhs,
                             int                    rhsComp,
                             const MultiFab* const* betan, 
                             const MultiFab* const* betanp1,
                             int                    betaComp)
{
  if (verbose) amrex::Print() << "... Diffusion::diffuse_velocity() lev: " << level << std::endl;

    const Real strt_time = ParallelDescriptor::second();

    int allnull, allthere;
    checkBetas(betan, betanp1, allthere, allnull);
    
    if (allnull) {
	amrex::Abort("Diffusion::diffuse_velocity(): Constant viscosity case no longer supported");
    }

    BL_ASSERT(allthere);

    BL_ASSERT( rho_flag == 1 || rho_flag == 3);

    // FIXME? min fails for face-centered EB MFs
    // test another way?
// #ifdef AMREX_DEBUG
//     for (int d = 0; d < BL_SPACEDIM; ++d)
//         BL_ASSERT( betan[d]->min(0,0) >= 0.0 );
// #endif

    diffuse_tensor_velocity(dt,be_cn_theta,rho_half,rho_flag,
			    delta_rhs,rhsComp,betan,betanp1,betaComp);

    if (verbose)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

	amrex::Print() << "Diffusion::diffuse_velocity(): lev: " << level
		       << ", time: " << run_time << '\n';
    }
}

void
Diffusion::diffuse_tensor_velocity (Real                   dt,
                                    Real                   be_cn_theta,
                                    const MultiFab&        rho_half,
                                    int                    rho_flag, 
                                    MultiFab*              delta_rhs,
                                    int                    rhsComp,
                                    const MultiFab* const* betan, 
                                    const MultiFab* const* betanp1,
                                    int                    betaComp)
{
    BL_ASSERT(rho_flag == 1 || rho_flag == 3);
    const int finest_level = parent->finestLevel();
    const MultiFab& volume = navier_stokes->Volume();
    //
    // At this point, S_old has bndry at time N S_new contains GRAD(SU).
    //
    MultiFab&  U_old     = navier_stokes->get_old_data(State_Type);
    MultiFab&  U_new     = navier_stokes->get_new_data(State_Type);
    const Real cur_time  = navier_stokes->get_state_data(State_Type).curTime();
    const Real prev_time = navier_stokes->get_state_data(State_Type).prevTime();

    int allnull, allthere;
    checkBetas(betan, betanp1, allthere, allnull);
    //
    // U_new now contains the inviscid update of U.
    // This is part of the RHS for the viscous solve.
    //
    const int soln_ng = 1;
    MultiFab Rhs(grids,dmap,BL_SPACEDIM,0, MFInfo(),navier_stokes->Factory());
    MultiFab Soln(grids,dmap,BL_SPACEDIM,soln_ng,MFInfo(),navier_stokes->Factory());
    MultiFab** tensorflux_old;
    FluxBoxes fb_old;

    const MultiFab* area   = navier_stokes->Area();
    // need for computeBeta. unsure why computeBeta defines area in this way
    // or why it even bothers to pass area when it's also passing geom
    const MultiFab *ap[AMREX_SPACEDIM];
    for (int d=0; d<AMREX_SPACEDIM; ++d)
    {
	ap[d] = &(area[d]);
    }
    // this may need to be 1 for EB?
    // could get this by tensorflux_old[0]->nGrow()
    int flux_ng = 0;

    // fixme
    // MultiFab** tf_old;
    // MultiFab** tensorflux;
    
    //FIXME for debugging
    // MultiFab Rhs2(grids,dmap,BL_SPACEDIM,0, MFInfo(),navier_stokes->Factory());
    // static int count=0; count++;
    
    //
    // Set up Rhs.
    //
    if ( be_cn_theta != 1 )
    {
      //
      // Compute time n viscous terms
      //     
      const Real a = 0.0;
      Real       b = -(1.0-be_cn_theta)*dt;

      if (allnull)
	b *= visc_coef[Xvel];
      
      // MLMG tensor solver
      {	
	LPInfo info;
	info.setAgglomeration(agglomeration);
	info.setConsolidation(consolidation);
	info.setMaxCoarseningLevel(0);
	info.setMetricTerm(false);

#ifdef AMREX_USE_EB
	const auto& ebf = &dynamic_cast<EBFArrayBoxFactory const&>(navier_stokes->Factory());
	MLEBTensorOp tensorop({navier_stokes->Geom()}, {grids}, {dmap}, info, {ebf});
#else
	MLTensorOp tensorop({navier_stokes->Geom()}, {grids}, {dmap}, info);
#endif
	
	tensorop.setMaxOrder(tensor_max_order);
	
	// create right container
	Array<LinOpBCType,AMREX_SPACEDIM> mlmg_lobc[AMREX_SPACEDIM];
	Array<LinOpBCType,AMREX_SPACEDIM> mlmg_hibc[AMREX_SPACEDIM];
	// fill it
	for (int i=0; i<AMREX_SPACEDIM; i++)
	  setDomainBC(mlmg_lobc[i], mlmg_hibc[i], Xvel+i);
	// pass to op
	tensorop.setDomainBC({AMREX_D_DECL(mlmg_lobc[0],mlmg_lobc[1],mlmg_lobc[2])},
			     {AMREX_D_DECL(mlmg_hibc[0],mlmg_hibc[1],mlmg_hibc[2])});
	
	// set coarse-fine BCs
	{	    
	  MultiFab crsedata;
	  int ng = soln_ng;
	  
	  if (level > 0) {
	    auto& crse_ns = *(coarser->navier_stokes);
	    crsedata.define(crse_ns.boxArray(), crse_ns.DistributionMap(),
			    AMREX_SPACEDIM, ng, MFInfo(), crse_ns.Factory());
	    AmrLevel::FillPatch(crse_ns, crsedata, ng, prev_time, State_Type, Xvel,
				AMREX_SPACEDIM);
	    
	    tensorop.setCoarseFineBC(&crsedata, crse_ratio[0]);
	  }
	    
	  AmrLevel::FillPatch(*navier_stokes,Soln,ng,prev_time,State_Type,Xvel,AMREX_SPACEDIM);
	  
	  // fixme? Do we need/want this
	  // seems like this ought be to have been done in FillPatch...
	  // EB_set_covered(Soln, 0, AMREX_SPACEDIM, ng, 1.2345e30);
	  ///

	  tensorop.setLevelBC(0, &Soln);
	  
	  // FIXME: check divergence of vel
	  // MLNodeLaplacian mllap({navier_stokes->Geom()}, {grids}, {dmap}, info);
	  // mllap.setDomainBC(mlmg_lobc[0], mlmg_hibc[0]);
	  // Rhs2.setVal(0.);
	  // mllap.compDivergence({&Rhs2}, {&Soln});
	  // amrex::WriteSingleLevelPlotfile("div_"+std::to_string(count), Rhs2, {AMREX_D_DECL("x","y","z")},navier_stokes->Geom(), 0.0, 0);
	  //
	}

	tensorop.setScalars(a, b);
	
	Array<MultiFab,AMREX_SPACEDIM> face_bcoef;
	for (int n = 0; n < BL_SPACEDIM; n++)
	{
	  face_bcoef[n].define(area[n].boxArray(),area[n].DistributionMap(),1,0,MFInfo(),navier_stokes->Factory());
	}
	computeBeta(face_bcoef,betan,betaComp,navier_stokes->Geom(),ap,
		    parent->Geom(0).IsRZ());
	
	tensorop.setShearViscosity(0, amrex::GetArrOfConstPtrs(face_bcoef));
	// FIXME??? Hack 
	// remove the "divmusi" terms by setting kappa = (2/3) mu
	//
	Print()<<"WARNING: Hack to get rid of divU terms ...\n";
	Array<MultiFab,AMREX_SPACEDIM> kappa;
	Real twothirds = 2.0/3.0;
	for (int idim = 0; idim < AMREX_SPACEDIM; ++idim)
	{
	  kappa[idim].define(face_bcoef[idim].boxArray(), face_bcoef[idim].DistributionMap(), 1, 0, MFInfo(),navier_stokes->Factory());
	  MultiFab::Copy(kappa[idim], face_bcoef[idim], 0, 0, 1, 0);
	  kappa[idim].mult(twothirds);
	}
	// bulk viscosity not ususally needed for gasses
	tensorop.setBulkViscosity(0, amrex::GetArrOfConstPtrs(kappa));

#ifdef AMREX_USE_EB
	MultiFab cc_bcoef(grids,dmap,BL_SPACEDIM,0,MFInfo(),navier_stokes->Factory());
	EB_average_face_to_cellcenter(cc_bcoef, 0,
				      amrex::GetArrOfConstPtrs(face_bcoef));
	tensorop.setEBShearViscosity(0, cc_bcoef);
	// part of hack to remove divmusi terms
	cc_bcoef.mult(twothirds);
	tensorop.setEBBulkViscosity(0, cc_bcoef);
#endif
	  
	MLMG mlmg(tensorop);
	// FIXME -- consider making new parameters max_iter and bottom_verbose
	//mlmg.setMaxIter(max_iter);
	mlmg.setMaxFmgIter(max_fmg_iter);
	mlmg.setVerbose(10);
	mlmg.setBottomVerbose(10);
	//mlmg.setBottomVerbose(bottom_verbose);
	  
	mlmg.apply({&Rhs}, {&Soln});

	if (do_reflux && (level<finest_level || level>0))
	{
	  tensorflux_old = fb_old.define(navier_stokes, AMREX_SPACEDIM);
	  //fixme --- after debugging go back to fluxbox fb_old
	  //tensorflux_old = new MultiFab*[BL_SPACEDIM];
	  // for (int dir = 0; dir < BL_SPACEDIM; dir++)
	  // {
	  //   const BoxArray& ba = navier_stokes->getEdgeBoxArray(dir);
	  //   const DistributionMapping& dm = navier_stokes->DistributionMap();
	  //   tensorflux_old[dir] = new MultiFab(ba,dm,AMREX_SPACEDIM,0);
	  // }

	  std::array<MultiFab*,AMREX_SPACEDIM> fp{AMREX_D_DECL(tensorflux_old[0], tensorflux_old[1], tensorflux_old[2])};
	  mlmg.getFluxes({fp},{&Soln});

	  for (int d = 0; d < BL_SPACEDIM; d++){
	    for (int i = 0; i < BL_SPACEDIM; ++i){
	      // manage area-weighting in final total flux computation,
	      // so only have to deal with EB switches once
	      //MultiFab::Multiply(*tensorflux_old[d],area[d],0,i,1,flux_ng);
	      tensorflux_old[d]->mult(-b/dt,i,1,flux_ng);
	    }
	  }
	  //FIXME
	  // static int count=0; count++;
	  // VisMF::Write( *tensorflux_old[0],"fluxx_"+std::to_string(count));
	  // VisMF::Write( *tensorflux_old[1],"fluxy_"+std::to_string(count));
	  // {
	  //   // read in result MF from unaltered version of code
	  //   std::string name2="../run2d/fluxx_"+std::to_string(count);
	  //   std::cout << "Reading " << name2 << std::endl;
	  //   MultiFab mf2(tensorflux_old[0]->boxArray(),dmap,tensorflux_old[0]->nComp(),tensorflux_old[0]->nGrow());
	  //   VisMF::Read(mf2, name2);
	  //   MultiFab mfdiff(mf2.boxArray(), dmap, mf2.nComp(), mf2.nGrow());
	  //   // Diff local MF and MF from unaltered code 
	  //   MultiFab::Copy(mfdiff, *tensorflux_old[0], 0, 0, mfdiff.nComp(), mfdiff.nGrow());
	  //   mfdiff.minus(mf2, 0, mfdiff.nComp(), mfdiff.nGrow());

	  //   for (int icomp = 0; icomp < mfdiff.nComp(); ++icomp) {
	  //     std::cout << "Min and max of the diff are " << mfdiff.min(icomp,mf2.nGrow()) 
	  // 		<< " and " << mfdiff.max(icomp,mf2.nGrow());
	  //     if (mfdiff.nComp() > 1) {
	  // 	std::cout << " for component " << icomp;
	  //     }
	  //     std::cout << "." << std::endl;
	  //   }
	  //   // write out difference MF for viewing: amrvis -mf 
	  //   std::cout << "Writing mfdiff" << std::endl;
	  //   VisMF::Write(mfdiff, "flxxdiff"+std::to_string(count));
	  // }
	  // amrex::WriteSingleLevelPlotfile("fluxx_"+std::to_string(count), *tensorflux_old[0], {AMREX_D_DECL("x","y","z")},navier_stokes->Geom(), 0.0, 0);
	  // amrex::WriteSingleLevelPlotfile("fluxy_"+std::to_string(count), *tensorflux_old[1], {AMREX_D_DECL("x","y","z")},navier_stokes->Geom(), 0.0, 0);
	  // amrex::WriteSingleLevelPlotfile("fluxx_"+std::to_string(count), *tensorflux_old[0], {AMREX_D_DECL("x","y","z")},navier_stokes->Geom(), 0.0, 0);
	  // //FIXME
 	  // Print()<<"Old tensor fluxes ...\n";
	  // amrex::print_state(*tensorflux_old[0], {64,80});
	  // amrex::print_state(*tensorflux_old[1], {64,80});
	  //
	}
      }
      /*
# if 0
      // Old Tensor solve
    {
      const int soln_old_grow = 1;
      MultiFab Soln_old(grids,dmap,BL_SPACEDIM,soln_old_grow);
      ViscBndryTensor visc_bndry;
      
      const MultiFab& rho = (rho_flag == 1) ? rho_half : navier_stokes->rho_ptime;
      
      {
	std::unique_ptr<DivVis> tensor_op
	  (getTensorOp(a,b,prev_time,visc_bndry,rho,betan,betaComp));
	tensor_op->maxOrder(tensor_max_order);
	//
	// Copy to single-component multifab.  Use Soln as a temporary here.
	//
	MultiFab::Copy(Soln_old,U_old,Xvel,0,BL_SPACEDIM,0);
	
	tensor_op->apply(Rhs2,Soln_old);
	
	if (do_reflux && (level<finest_level || level>0))
	{
	  tf_old = fb_old.define(navier_stokes, BL_SPACEDIM);
	  tensor_op->compFlux(D_DECL(*(tf_old[0]),
				     *(tf_old[1]),
				     *(tf_old[2])),Soln_old);
	  for (int d = 0; d < BL_SPACEDIM; d++)
	    tf_old[d]->mult(-b/(dt*navier_stokes->Geom().CellSize()[d]),0);
	}
      }
      
      Soln_old.clear();
    }
#endif
      */
    // fixme -- compare fluxes
    // MultiFab** tmp = new MultiFab*[BL_SPACEDIM]; 
    // for (int dir = 0; dir < BL_SPACEDIM; dir++)
    // {
    //   const BoxArray& ba = navier_stokes->getEdgeBoxArray(dir);
    //   const DistributionMapping& dm = navier_stokes->DistributionMap();
    //   tmp[dir] = new MultiFab(ba,dm,AMREX_SPACEDIM,0);
    //   MultiFab::Copy(*tmp[dir],*tensorflux_old[dir],0,0,AMREX_SPACEDIM,0);
    //   MultiFab::Subtract(*tmp[dir],*tf_old[dir],0,0,AMREX_SPACEDIM,0);
    //   VisMF::Write(*tmp[dir],"tf"+std::to_string(dir));

    //   Vector<Real> nrm0,nrm1,nrm2;
    // 	  Real n0=0.,n1=0.,n2=0.;
    // 	  nrm0 = tmp[dir]->norm0({AMREX_D_DECL(0,1,2)});
    // 	  nrm1 = tmp[dir]->norm1({AMREX_D_DECL(0,1,2)});
    // 	  nrm2 = tmp[dir]->norm2({AMREX_D_DECL(0,1,2)});
    // 	  for (int i = 0; i<AMREX_SPACEDIM; i++){
    // 	    n0=max(nrm0[i],n0);
    // 	    n1+=nrm1[i];
    // 	    n2+=nrm2[i];
    // 	  }
    // 	  n1*=pow(navier_stokes->Geom().CellSize()[0],AMREX_SPACEDIM)/AMREX_SPACEDIM;
    // 	  n2*=pow(navier_stokes->Geom().CellSize()[0],AMREX_SPACEDIM)/AMREX_SPACEDIM;
    // 	  Print()<<(navier_stokes->Geom().Domain().hiVect())[0]+1<<" "
    // 	  	  <<navier_stokes->Geom().CellSize()[0]<<" "
    // 	  	  <<n0<<" "<<n1<<" "<<n2<<" \n";
    // 	  std::ofstream datafile;
    // 	  datafile.open("fluxDiff"+std::to_string(dir)+".txt", std::ofstream::out | std::ofstream::app);
    // 	  datafile<<(navier_stokes->Geom().Domain().hiVect())[0]+1<<" "
    // 	  	  <<navier_stokes->Geom().CellSize()[0]<<" "
    // 	  	  <<n0<<" "<<n1<<" "<<n2<<" \n";
    // 	  datafile.close();

    // }
    
    
    //    amrex::WriteSingleLevelPlotfile("rhsOldA_"+std::to_string(count), Rhs2, {AMREX_D_DECL("x","y","z")},navier_stokes->Geom(), 0.0, 0);
    
#if (BL_SPACEDIM == 2) 
    if (parent->Geom(0).IsRZ())
    {
#ifdef AMREX_USE_EB
      amrex::Abort("tensor r-z still under development. \n");
#endif
      // R-Z still needs old volume weighting
      // need to check above to make sure vol factor is in there (beta, fluxes)
      // and then below should be ok as is
      
      int fort_xvel_comp = Xvel+1;

#ifdef _OPENMP
#pragma omp parallel
#endif
      for (MFIter Rhsmfi(Rhs,true); Rhsmfi.isValid(); ++Rhsmfi)
      {
        const Box& bx     = Rhsmfi.tilebox();

        const Box& rbx    = Rhsmfi.validbox();
        FArrayBox& rhsfab = Rhs[Rhsmfi];

        const Box& sbx    = U_old[Rhsmfi].box();
        Vector<Real> rcen(bx.length(0));
        navier_stokes->Geom().GetCellLoc(rcen, bx, 0);
        const int*       lo        = bx.loVect();
        const int*       hi        = bx.hiVect();
        const int*       rlo       = rbx.loVect();
        const int*       rhi       = rbx.hiVect();
        const int*       slo       = sbx.loVect();
        const int*       shi       = sbx.hiVect();
        Real*            rhs       = rhsfab.dataPtr();
        const Real*      sdat      = U_old[Rhsmfi].dataPtr(Xvel);
        const Real*      rcendat   = rcen.dataPtr();
        const Real       coeff     = (1.0-be_cn_theta)*dt;
        const Real*      voli      = volume[Rhsmfi].dataPtr();
        Box              vbox      = volume[Rhsmfi].box();
        const int*       vlo       = vbox.loVect();
        const int*       vhi       = vbox.hiVect();
        const FArrayBox& betax     = (*betanp1[0])[Rhsmfi];
        const int*       betax_lo  = betax.loVect();
        const int*       betax_hi  = betax.hiVect();
        const Real*      betax_dat = betax.dataPtr(betaComp);
        const FArrayBox& betay     = (*betanp1[1])[Rhsmfi];
        const int*       betay_lo  = betay.loVect();
        const int*       betay_hi  = betay.hiVect();
        const Real*      betay_dat = betay.dataPtr(betaComp);

        tensor_hooprhs(&fort_xvel_comp,
		       ARLIM(lo), ARLIM(hi),
		       rhs, ARLIM(rlo), ARLIM(rhi), 
		       sdat, ARLIM(slo), ARLIM(shi),
		       rcendat, &coeff, 
		       voli, ARLIM(vlo), ARLIM(vhi),
		       betax_dat,ARLIM(betax_lo),ARLIM(betax_hi),
		       betay_dat,ARLIM(betay_lo),ARLIM(betay_hi));
      }
    }
#endif
    }
    else
    {
      Rhs.setVal(0.);
    }
    	  //fixme -- for RZ, test MLMG metric terms 
	  // amrex::WriteSingleLevelPlotfile("rhsMLMG_"+std::to_string(count), Rhs, {AMREX_D_DECL("x","y","z")},navier_stokes->Geom(), 0.0, 0);
	  // amrex::WriteSingleLevelPlotfile("rhsOld_"+std::to_string(count), Rhs2, {AMREX_D_DECL("x","y","z")},navier_stokes->Geom(), 0.0, 0);
	  // MultiFab::Copy(Soln,Rhs,0,0,AMREX_SPACEDIM,0);
	  // MultiFab::Subtract(Soln,Rhs2,0,0,AMREX_SPACEDIM,0);
	  // amrex::WriteSingleLevelPlotfile("diff_"+std::to_string(count), Soln, {AMREX_D_DECL("x","y","z")},navier_stokes->Geom(), 0.0, 0);
	  // MultiFab::Divide(Soln,Rhs2,0,0,AMREX_SPACEDIM,0);
	  // amrex::WriteSingleLevelPlotfile("rdiff_"+std::to_string(count), Soln, {AMREX_D_DECL("x","y","z")},navier_stokes->Geom(), 0.0, 0);
	  //amrex::Abort("check rhs");


    
    //
    // Complete Rhs by adding body sources.
    //
#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter Rhsmfi(Rhs,true); Rhsmfi.isValid(); ++Rhsmfi)
    {
      const Box& bx     = Rhsmfi.tilebox();
      FArrayBox& rhsfab = Rhs[Rhsmfi];
      FArrayBox& Ufab   = U_new[Rhsmfi];

      //
      // Don't Scale inviscid part by volume anymore.
      //
      for (int comp = 0; comp < BL_SPACEDIM; comp++)
      {
        const int sigma = Xvel + comp;

	// No more vol scaling
        //Ufab.mult(volume[Rhsmfi],bx,0,sigma,1);
        //
        // Multiply by density at time nph (if rho_flag==1)
        //                     or time n   (if rho_flag==3).
        //
        if (rho_flag == 1)
          Ufab.mult(rho_half[Rhsmfi],bx,0,sigma,1);
        if (rho_flag == 3)
          Ufab.mult((navier_stokes->rho_ptime)[Rhsmfi],bx,0,sigma,1);
        //
        // Add to Rhs which contained operator applied to U_old.
        //
        rhsfab.plus(Ufab,bx,sigma,comp,1);

        if (delta_rhs != 0)
        {
          FArrayBox& deltafab = (*delta_rhs)[Rhsmfi];
          deltafab.mult(dt,bx,comp+rhsComp,1);
          //deltafab.mult(volume[Rhsmfi],bx,0,comp+rhsComp,1);
          rhsfab.plus(deltafab,bx,comp+rhsComp,comp,1);
        }
      }
    }
    
    //
    // Construct viscous operator at time N+1.
    //
    const Real a = 1.0;
    Real       b = be_cn_theta*dt;
    if (allnull)
        b *= visc_coef[Xvel];

    // MLMG solution
    {
      // genaric tol suggestion for MLMG
      // const Real tol_rel = 1.e-11;
      // const Real tol_abs = 0.0;
      // cribbing from scalar
      const Real tol_rel = visc_tol;
      const Real tol_abs = get_scaled_abs_tol(Rhs, visc_tol);
      
      LPInfo info;
      info.setAgglomeration(agglomeration);
      info.setConsolidation(consolidation);
      //fixme??
      info.setMetricTerm(false);
      info.setMaxCoarseningLevel(100);

#ifdef AMREX_USE_EB
      const auto& ebf = &dynamic_cast<EBFArrayBoxFactory const&>(navier_stokes->Factory());
      MLEBTensorOp tensorop({navier_stokes->Geom()}, {grids}, {dmap}, info, {ebf});
      
      std::array<const amrex::MultiCutFab*,AMREX_SPACEDIM>areafrac = ebf->getAreaFrac();
#else
      MLTensorOp tensorop({navier_stokes->Geom()}, {grids}, {dmap}, info);
#endif
      
      tensorop.setMaxOrder(tensor_max_order);

      // create right container
      Array<LinOpBCType,AMREX_SPACEDIM> mlmg_lobc[AMREX_SPACEDIM];
      Array<LinOpBCType,AMREX_SPACEDIM> mlmg_hibc[AMREX_SPACEDIM];
      // fill it
      for (int i=0; i<AMREX_SPACEDIM; i++)
	setDomainBC(mlmg_lobc[i], mlmg_hibc[i], Xvel+i);
      // pass to op
      tensorop.setDomainBC({AMREX_D_DECL(mlmg_lobc[0],mlmg_lobc[1],mlmg_lobc[2])},
			   {AMREX_D_DECL(mlmg_hibc[0],mlmg_hibc[1],mlmg_hibc[2])});

      // set up level BCs
      {
	MultiFab crsedata;
	int ng = soln_ng;
	
	if (level > 0) {
	  auto& crse_ns = *(coarser->navier_stokes);
	  crsedata.define(crse_ns.boxArray(), crse_ns.DistributionMap(), AMREX_SPACEDIM,
			  ng, MFInfo(),crse_ns.Factory());
	  AmrLevel::FillPatch(crse_ns, crsedata, ng, cur_time, State_Type, Xvel,
			      AMREX_SPACEDIM);
	  tensorop.setCoarseFineBC(&crsedata, crse_ratio[0]);
	}
	    
	AmrLevel::FillPatch(*navier_stokes,Soln,ng,cur_time,State_Type,Xvel,AMREX_SPACEDIM);
	
	tensorop.setLevelBC(0, &Soln);
      }

      {
	MultiFab acoef;
	//fixme? not certain how many ghost cells
	acoef.define(grids, dmap, 1, 1, MFInfo(), navier_stokes->Factory());
	std::pair<Real,Real> scalars;
	// fixme? don't know why we're bothering with rhsscale....
	Real rhsscale = 1.0;
	const MultiFab& rho = (rho_flag == 1) ? rho_half : navier_stokes->rho_ctime;
	// computeAlpha(acoef, scalars, Xvel, a, b, cur_time, rho, 1,
	// 	     &rhsscale, 0, nullptr);

	// static void computeAlpha (amrex::MultiFab&       alpha,
        //                       std::pair<amrex::Real,amrex::Real>& scalars,
        //                       amrex::Real            a,
        //                       amrex::Real            b,
        //                       const amrex::MultiFab& rho_half,
        //                       int                    rho_flag, 
        //                       amrex::Real*           rhsscale,
        //                       const amrex::MultiFab* alpha_in,
        //                       int                    alpha_in_comp,
        //                       const amrex::MultiFab* rho,
        //                       int                    rho_comp,
        //                       const amrex::Geometry& geom,
        //                       const amrex::MultiFab& volume,
        //                       bool                   use_hoop_stress);
	computeAlpha(acoef, scalars, a, b, rho,
		     1, &rhsscale, nullptr, 0, 
		     nullptr, 0, // this 2nd rho doesn't get used b/c rho_flag==1
		     navier_stokes->Geom(),volume,parent->Geom(0).IsRZ());

	tensorop.setScalars(scalars.first, scalars.second);
	tensorop.setACoeffs(0, acoef);
      }
      
      {
	Array<MultiFab,AMREX_SPACEDIM> face_bcoef;
	for (int n = 0; n < BL_SPACEDIM; n++)
	{
	  face_bcoef[n].define(area[n].boxArray(),area[n].DistributionMap(),1,0,MFInfo(),navier_stokes->Factory());
	}

	//	computeBeta(face_bcoef,betan,betaComp);
	computeBeta(face_bcoef,betan,betaComp,navier_stokes->Geom(),ap,
		    parent->Geom(0).IsRZ());
	tensorop.setShearViscosity(0, amrex::GetArrOfConstPtrs(face_bcoef));
	// FIXME??? Hack 
	// remove the "divmusi" terms by setting kappa = (2/3) mu
	//
	Print()<<"WARNING: Hack to get rid of divU terms ...\n";
	Array<MultiFab,AMREX_SPACEDIM> kappa;
	Real twothirds = 2.0/3.0;
	for (int idim = 0; idim < AMREX_SPACEDIM; ++idim)
	{
	  kappa[idim].define(face_bcoef[idim].boxArray(), face_bcoef[idim].DistributionMap(), 1, 0, MFInfo(),navier_stokes->Factory());
	  MultiFab::Copy(kappa[idim], face_bcoef[idim], 0, 0, 1, 0);
	  kappa[idim].mult(twothirds);
	}
	// bulk viscosity not ususally needed for gasses
	tensorop.setBulkViscosity(0, amrex::GetArrOfConstPtrs(kappa));

#ifdef AMREX_USE_EB
	MultiFab cc_bcoef(grids,dmap,BL_SPACEDIM,0,MFInfo(),navier_stokes->Factory());
	EB_average_face_to_cellcenter(cc_bcoef, 0,
				      amrex::GetArrOfConstPtrs(face_bcoef));
	tensorop.setEBShearViscosity(0, cc_bcoef);
	// part of hack to remove divmusi terms
	cc_bcoef.mult(twothirds);
	tensorop.setEBBulkViscosity(0, cc_bcoef);
#endif
      }
	  
      MLMG mlmg(tensorop);
      //fixme?
      //mlmg.setMaxIter(max_iter);
      mlmg.setMaxFmgIter(max_fmg_iter);
      mlmg.setVerbose(verbose);
      //mlmg.setBottomVerbose(bottom_verbose);
      
      // ensures ghost cells of sol are correctly filled when returned from solver
      //fixme?? isn't FillPatch ususally the way to do it?
      // would need not to copy ghost cells to U_new below
      mlmg.setFinalFillBC(true);

      //    solution.setVal(0.0);
      mlmg.solve({&Soln}, {&Rhs}, tol_rel, tol_abs);
      
      //
      // Copy into state variable at new time.
      //
      MultiFab::Copy(U_new,Soln,0,Xvel,AMREX_SPACEDIM,soln_ng);
      //FIXME check soln
       //static int count = 0; count++;
       //amrex::WriteSingleLevelPlotfile("ds_"+std::to_string(count), U_new, {AMREX_D_DECL("x","y","z"),"den","trac"},navier_stokes->Geom(), 0.0, 0);

      //
      // Modify diffusive fluxes here.
      //
      if (do_reflux && (level < finest_level || level > 0))
      {
	//Print()<<"Doing reflux ...\n";
	  
	FluxBoxes fb(navier_stokes, BL_SPACEDIM);
	MultiFab** tensorflux = fb.get();
	//fixme --- for debugging
	//tensorflux = new MultiFab*[BL_SPACEDIM];
	// for (int dir = 0; dir < BL_SPACEDIM; dir++)
	// {
	//   const BoxArray& ba = navier_stokes->getEdgeBoxArray(dir);
	//   const DistributionMapping& dm = navier_stokes->DistributionMap();
	//   tensorflux[dir] = new MultiFab(ba,dm,AMREX_SPACEDIM,0);
	// }
	std::array<MultiFab*,AMREX_SPACEDIM> fp{AMREX_D_DECL(tensorflux[0], tensorflux[1], tensorflux[2])};	
	mlmg.getFluxes({fp},{&Soln});
	
#ifdef AMREX_USE_EB
#ifdef _OPENMP
#pragma omp parallel
#endif
      for (MFIter mfi(Soln,true); mfi.isValid(); ++mfi)
      {  
	Box bx = mfi.tilebox();
	
	// need face-centered tilebox for each direction
	D_TERM(const Box& xbx = mfi.tilebox(IntVect::TheDimensionVector(0));,
	       const Box& ybx = mfi.tilebox(IntVect::TheDimensionVector(1));,
	       const Box& zbx = mfi.tilebox(IntVect::TheDimensionVector(2)););
	std::array<const Box*,AMREX_SPACEDIM> nbx{AMREX_D_DECL(&xbx,&ybx,&zbx)};
	
	// this is to check efficiently if this tile contains any eb stuff
	const EBFArrayBox& in_fab = static_cast<EBFArrayBox const&>(Soln[mfi]);
	const EBCellFlagFab& flags = in_fab.getEBCellFlagFab();
      
      if(flags.getType(amrex::grow(bx, flux_ng)) == FabType::covered)
      {
	// If tile is completely covered by EB geometry, set 
	// value to some very large number so we know if
	// we accidentaly use these covered vals later in calculations
	D_TERM(tensorflux[0]->setVal(1.2345e30, xbx, 0, AMREX_SPACEDIM);,
	       tensorflux[1]->setVal(1.2345e30, ybx, 0, AMREX_SPACEDIM);,
	       tensorflux[2]->setVal(1.2345e30, zbx, 0, AMREX_SPACEDIM););
      }
      else
      {
	// No cut cells in tile + flux_ng-cell witdh halo -> use non-eb routine
	if(flags.getType(amrex::grow(bx, flux_ng)) == FabType::regular)
        {
	  for (int i = 0; i < BL_SPACEDIM; ++i)
	  {
	    (*tensorflux[i])[mfi].mult(b/dt,*nbx[i],0,AMREX_SPACEDIM);
	    if ( be_cn_theta!=1 ) 
	      (*tensorflux[i])[mfi].plus((*tensorflux_old[i])[mfi],*nbx[i],0,0,BL_SPACEDIM);
	    for (int d = 0; d < BL_SPACEDIM; ++d)
	      (*tensorflux[i])[mfi].mult(area[i][mfi],*nbx[i],0,d,1);
	  }
	}
        else
        {
	  // Use EB routines
          for (int i = 0; i < BL_SPACEDIM; ++i)
          {
            (*tensorflux[i])[mfi].mult(b/dt,*nbx[i],0,AMREX_SPACEDIM);
	    if ( be_cn_theta!=1 ) 	    
	      (*tensorflux[i])[mfi].plus((*tensorflux_old[i])[mfi],*nbx[i],0,0,BL_SPACEDIM);
	    for (int d = 0; d < BL_SPACEDIM; ++d) {
	      (*tensorflux[i])[mfi].mult(area[i][mfi],*nbx[i],0,d,1);
	      (*tensorflux[i])[mfi].mult((*areafrac[i])[mfi],*nbx[i],0,d,1);   
	    }
	  }
        }
      }        
    }
#else
    // Non-EB
    {
      // Here we keep the weighting by the volume for non-EB && R-Z case
      // The flag has already been checked for only 2D at the begining of the routine
      if (parent->Geom(0).IsRZ()) //add_hoop_stress)
      {
	for (int d = 0; d < BL_SPACEDIM; ++d)
	  // FIXME -- need to add time n flux, if (be_cn_theta!=1)
	  tensorflux[d]->mult(b/(dt * navier_stokes->Geom().CellSize()[d]),0,AMREX_SPACEDIM,flux_ng);
      }
      else // Generic case for non-EB and 2D or 3D Cartesian
      {
	for (int d = 0; d < BL_SPACEDIM; d++){
	  tensorflux[d]->mult(b/dt,0,AMREX_SPACEDIM,flux_ng);
	  if ( be_cn_theta!=1 ) 
	    tensorflux[d]->plus(*(tensorflux_old[d]),0,BL_SPACEDIM,flux_ng);
	  for (int i = 0; i < BL_SPACEDIM; ++i)
	    MultiFab::Multiply(*tensorflux[d],area[d],0,i,1,flux_ng);
	}
      }      
    }
#endif
	/////////////////////////////////////////
	if (level > 0)
        {
	  for (int k = 0; k < BL_SPACEDIM; k++)
	    viscflux_reg->FineAdd(*(tensorflux[k]),k,Xvel,Xvel,BL_SPACEDIM,dt);
	}

	if (level < finest_level)
        {
	  for (int d = 0; d < BL_SPACEDIM; d++)
	    finer->viscflux_reg->CrseInit(*tensorflux[d],d,0,Xvel,BL_SPACEDIM,-dt);
	}
      }
    }
    /*    
#if 0
    // Old tensor solve
   
    const int soln_grow = 1;
    //MultiFab Soln(grids,dmap,BL_SPACEDIM,soln_grow);
    Soln.setVal(0);
    //
    // Compute guess of solution.
    //
    if (level == 0)
    {
        MultiFab::Copy(Soln,U_old,Xvel,0,BL_SPACEDIM,0);
    }
    else
    {
        navier_stokes->FillCoarsePatch(Soln,0,cur_time,State_Type,Xvel,BL_SPACEDIM);
    }
    //
    // Copy guess into U_new.
    //
    // The new-time operator is initialized with a "guess" for the new-time
    // state.  We intentionally initialize the grow cells with a bogus
    // value to emphasize that the values are not to be considered "valid"
    // (we shouldn't specify any grow cell information), but rather are to
    // filled by the "physics bc's, etc" in the problem-dependent code.  In
    // the course of this filling (typically while generating/filling the
    // BndryData object for the solvers), StateData::filcc is called to get
    // physical bc's.  Here 'something computable' has to already exist in
    // the grow cells (even though filcc ultimately will fill the corner
    // correctly, if applicable).  This is apparently where the
    // `something computable' is to be set.
    //
    int n_comp  = BL_SPACEDIM;
    int n_ghost = 1;
    U_new.setVal(BL_SAFE_BOGUS,Xvel,n_comp,n_ghost);
    n_ghost = 0;
    U_new.copy(Soln,0,Xvel,n_comp);
       
    ViscBndryTensor visc_bndry;
    const MultiFab& rho = (rho_flag == 1) ? rho_half : navier_stokes->rho_ctime;
    std::unique_ptr<DivVis> tensor_op
      (getTensorOp(a,b,cur_time,visc_bndry,rho,betanp1,betaComp));
    tensor_op->maxOrder(tensor_max_order);
    //
    // Construct solver and call it.
    //
    const Real S_tol     = visc_tol;
    const Real S_tol_abs = -1;
    if (use_tensor_cg_solve)
    {
        const int use_mg_pre = 0;
        MCCGSolver cg(*tensor_op,use_mg_pre);
        cg.solve(Soln,Rhs,S_tol,S_tol_abs);
    }
    else
    {
        MCMultiGrid mg(*tensor_op);
        mg.solve(Soln,Rhs,S_tol,S_tol_abs);
    }
    Rhs.clear();

    int visc_op_lev = 0;
    tensor_op->applyBC(Soln,visc_op_lev); // This may not be needed.
    //
    // Copy into state variable at new time.
    //
    n_ghost = soln_grow;
    MultiFab::Copy(U_new,Soln,0,Xvel,n_comp,n_ghost);
    //
    // Modify diffusive fluxes here.
    //
    FluxBoxes fb(navier_stokes, BL_SPACEDIM);
    MultiFab** tfnew = fb.get();

    if (do_reflux && (level < finest_level || level > 0))
    {
      // FluxBoxes fb(navier_stokes, BL_SPACEDIM);
      // MultiFab** tfnew = fb.get();
      tensor_op->compFlux(D_DECL(*(tfnew[0]), *(tfnew[1]), *(tfnew[2])),Soln);

      for (int d = 0; d < BL_SPACEDIM; d++)
      {
        tfnew[d]->mult(b/(dt*navier_stokes->Geom().CellSize()[d]),0);
        tfnew[d]->plus(*(tf_old[d]),0,BL_SPACEDIM,0);
      }       

      if (level > 0)
      {
        for (int k = 0; k < BL_SPACEDIM; k++)
        viscflux_reg->FineAdd(*(tfnew[k]),k,Xvel,Xvel,BL_SPACEDIM,dt);
      }

      if (level < finest_level)
      {
        for (int d = 0; d < BL_SPACEDIM; d++)
        finer->viscflux_reg->CrseInit(*tfnew[d],d,0,Xvel,BL_SPACEDIM,-dt);
       }
    }
#endif
    */
    // fixme -- compare fluxes
    // for (int dir = 0; dir < BL_SPACEDIM; dir++)
    // {
    //   MultiFab::Subtract(*tensorflux[dir],*tfnew[dir],0,0,AMREX_SPACEDIM,0);
    //   VisMF::Write(*tensorflux[dir],"tfnew"+std::to_string(dir));

    //   	  Vector<Real> nrm0,nrm1,nrm2;
    // 	  Real n0=0.,n1=0.,n2=0.;
    // 	  nrm0 = tensorflux[dir]->norm0({AMREX_D_DECL(0,1,2)});
    // 	  nrm1 = tensorflux[dir]->norm1({AMREX_D_DECL(0,1,2)});
    // 	  nrm2 = tensorflux[dir]->norm2({AMREX_D_DECL(0,1,2)});
    // 	  for (int i = 0; i<AMREX_SPACEDIM; i++){
    // 	    n0=max(nrm0[i],n0);
    // 	    n1+=nrm1[i];
    // 	    n2+=nrm2[i];
    // 	  }
    // 	  n1*=pow(navier_stokes->Geom().CellSize()[0],AMREX_SPACEDIM)/AMREX_SPACEDIM;
    // 	  n2*=pow(navier_stokes->Geom().CellSize()[0],AMREX_SPACEDIM)/AMREX_SPACEDIM;
    // 	  Print()<<(navier_stokes->Geom().Domain().hiVect())[0]+1<<" "
    // 	  	  <<navier_stokes->Geom().CellSize()[0]<<" "
    // 	  	  <<n0<<" "<<n1<<" "<<n2<<" \n";
    // 	  std::ofstream datafile;
    // 	  datafile.open("fluxtotDiff"+std::to_string(dir)+".txt", std::ofstream::out | std::ofstream::app);
    // 	  datafile<<(navier_stokes->Geom().Domain().hiVect())[0]+1<<" "
    // 	  	  <<navier_stokes->Geom().CellSize()[0]<<" "
    // 	  	  <<n0<<" "<<n1<<" "<<n2<<" \n";
    // 	  datafile.close();


    // }

    // amrex::Abort("check new fluxes");

}

void
Diffusion::diffuse_Vsync (MultiFab&              Vsync,
                          Real                   dt,
                          Real                   be_cn_theta,
                          const MultiFab&        rho_half,
                          int                    rho_flag,
                          const MultiFab* const* beta,
                          int                    betaComp,
			  bool                   update_fluxreg)
{
    BL_ASSERT(rho_flag == 1|| rho_flag == 3);

    int allnull, allthere;
    checkBeta(beta, allthere, allnull);

    //FIXME? min fails for face-centered EB mfs. 
    //
    // #ifdef AMREX_DEBUG
//     for (int d = 0; d < BL_SPACEDIM; ++d)
//         BL_ASSERT(allnull ? visc_coef[Xvel+d]>=0 : beta[d]->min(0,0) >= 0.0);
// #endif

    if (allnull)
      amrex::Abort("Constant viscosity case no longer supported");
//      diffuse_Vsync_constant_mu(Vsync,dt,be_cn_theta,rho_half,rho_flag,update_fluxreg);
    else
      diffuse_tensor_Vsync(Vsync,dt,be_cn_theta,rho_half,rho_flag,beta,betaComp,update_fluxreg);
    //
    // applyBC has put "incorrect" values in the ghost cells
    // outside external Dirichlet boundaries. Reset these to zero
    // so that syncproject and conservative interpolation works correctly.
    //
    Box domain = amrex::grow(navier_stokes->Geom().Domain(),1);

    for (int n = Xvel; n < Xvel+BL_SPACEDIM; n++)
    {
        const BCRec& velbc = navier_stokes->get_desc_lst()[State_Type].getBC(n);

        for (int k = 0; k < BL_SPACEDIM; k++)
        {
            if (velbc.hi(k) == EXT_DIR)
            {
                IntVect smallend = domain.smallEnd();
                smallend.setVal(k,domain.bigEnd(k));
                Box top_strip(smallend,domain.bigEnd(),IntVect::TheCellVector());
                Vsync.setVal(0,top_strip,n-Xvel,1,1);
            }
            if (velbc.lo(k) == EXT_DIR)
            {
                IntVect bigend = domain.bigEnd();
                bigend.setVal(k,domain.smallEnd(k));
                Box bottom_strip(domain.smallEnd(),bigend,IntVect::TheCellVector());
                Vsync.setVal(0,bottom_strip,n-Xvel,1,1);
            }
        }
    }
}

/*
//   may want to bring a constant viscosity option back later
//
void
Diffusion::diffuse_Vsync_constant_mu (MultiFab&       Vsync,
                                      Real            dt,
                                      Real            be_cn_theta,
                                      const MultiFab& rho_half,
                                      int             rho_flag,
				      bool            update_fluxreg)
{
  if (verbose) amrex::Print() << "Diffusion::diffuse_Vsync_constant_mu ...\n";

    const MultiFab& volume = navier_stokes->Volume();
    const MultiFab* area   = navier_stokes->Area();
    const Real*   dx       = navier_stokes->Geom().CellSize();
    //
    // At this point in time we can only do decoupled scalar
    // so we loop over components.
    //
    MultiFab Rhs(grids,dmap,1,0);

    for (int comp = 0; comp < BL_SPACEDIM; comp++)
    {
        MultiFab::Copy(Rhs,Vsync,comp,0,1,0);

        if (verbose > 1)
        {
            Real r_norm = Rhs.norm0();
	    amrex::Print() << "Original max of Vsync " << r_norm << '\n';
        }
        //
        // Multiply RHS by volume and density.
        //
        const MultiFab& rho = (rho_flag == 1) ? rho_half : navier_stokes->rho_ctime;
#ifdef _OPENMP
#pragma omp parallel
#endif
        for (MFIter Rhsmfi(Rhs,true); Rhsmfi.isValid(); ++Rhsmfi)
        {
	    const Box& bx = Rhsmfi.tilebox();
            Rhs[Rhsmfi].mult(volume[Rhsmfi],bx,0,0); 
            Rhs[Rhsmfi].mult(rho[Rhsmfi],bx,0,0); 
        }
        //
        // SET UP COEFFICIENTS FOR VISCOUS SOLVER.
        //
        const Real     a        = 1.0;
        const Real     b        = be_cn_theta*dt*visc_coef[comp];
        Real           rhsscale = 1.0;

        MultiFab Soln(grids,dmap,1,1);
        Soln.setVal(0);

        const Real S_tol     = visc_tol;
        const Real S_tol_abs = -1.0;
        
        LPInfo info;
        info.setAgglomeration(agglomeration);
        info.setConsolidation(consolidation);
        info.setMetricTerm(false);

        MLABecLaplacian mlabec({navier_stokes->Geom()}, {grids}, {dmap}, info);
        mlabec.setMaxOrder(max_order);

        std::array<LinOpBCType,AMREX_SPACEDIM> mlmg_lobc;
        std::array<LinOpBCType,AMREX_SPACEDIM> mlmg_hibc;
        setDomainBC(mlmg_lobc, mlmg_hibc, comp);
        
        mlabec.setDomainBC(mlmg_lobc, mlmg_hibc);
        if (level > 0) {
          mlabec.setCoarseFineBC(nullptr, crse_ratio[0]);
        }
        mlabec.setLevelBC(0, nullptr);

        {
          MultiFab acoef;
          std::pair<Real,Real> scalars;
          const Real cur_time = navier_stokes->get_state_data(State_Type).curTime();
          computeAlpha(acoef, scalars, comp, a, b, cur_time, rho, rho_flag,
                       &rhsscale, 0, nullptr);
          mlabec.setScalars(scalars.first, scalars.second);
          mlabec.setACoeffs(0, acoef);
        }
        
        {
          std::array<MultiFab,BL_SPACEDIM> bcoeffs;
          computeBeta(bcoeffs, nullptr, 0);
          mlabec.setBCoeffs(0, amrex::GetArrOfConstPtrs(bcoeffs));
        }

        MLMG mlmg(mlabec);
        if (use_hypre) {
          mlmg.setBottomSolver(MLMG::BottomSolver::hypre);
          mlmg.setBottomVerbose(hypre_verbose);
        }
        mlmg.setMaxFmgIter(max_fmg_iter);
        mlmg.setVerbose(verbose);

        Rhs.mult(rhsscale,0,1);

        mlmg.setFinalFillBC(true);
        mlmg.solve({&Soln}, {&Rhs}, S_tol, S_tol_abs);

        MultiFab::Copy(Vsync,Soln,0,comp,1,1);

        if (verbose > 1)
        {
            Real s_norm = Soln.norm0(0,Soln.nGrow());
	    amrex::Print() << "Final max of Vsync " << s_norm << '\n';
        }

        if (level > 0)
        {
          const DistributionMapping& dm = navier_stokes->DistributionMap();
	        MultiFab xflux(navier_stokes->getEdgeBoxArray(0), dm, 1, 0);
	        MultiFab yflux(navier_stokes->getEdgeBoxArray(1), dm, 1, 0);
#if (BL_SPACEDIM == 3)
	        MultiFab zflux(navier_stokes->getEdgeBoxArray(2), dm, 1, 0);
#endif	    

	        //
	        // The extra factor of dt comes from the fact that Vsync
	        // looks like dV/dt, not just an increment to V.
	        //
	        Real mult = -be_cn_theta*dt*dt*visc_coef[comp];

#ifdef _OPENMP
#pragma omp parallel
#endif
          for (MFIter Vsyncmfi(Vsync,true); Vsyncmfi.isValid(); ++Vsyncmfi)
          {
            const Box& xbx    = Vsyncmfi.nodaltilebox(0);
            const Box& ybx    = Vsyncmfi.nodaltilebox(1);
            FArrayBox& u_sync = Vsync[Vsyncmfi];
            const int* ulo    = u_sync.loVect();
            const int* uhi    = u_sync.hiVect();
		        FArrayBox& xff = xflux[Vsyncmfi];
            FArrayBox& yff = yflux[Vsyncmfi];
		
            DEF_LIMITS(xff,xflux_dat,xflux_lo,xflux_hi);
            DEF_LIMITS(yff,yflux_dat,yflux_lo,yflux_hi);
		
            const FArrayBox& xarea = area[0][Vsyncmfi];
            const FArrayBox& yarea = area[1][Vsyncmfi];
		
            DEF_CLIMITS(xarea,xarea_dat,xarea_lo,xarea_hi);
            DEF_CLIMITS(yarea,yarea_dat,yarea_lo,yarea_hi);

#if (BL_SPACEDIM == 2)
            viscsyncflux (u_sync.dataPtr(comp), ARLIM(ulo), ARLIM(uhi),
			                    xbx.loVect(), xbx.hiVect(),
			                    ybx.loVect(), ybx.hiVect(),
			                    xflux_dat,ARLIM(xflux_lo),ARLIM(xflux_hi),
			                    yflux_dat,ARLIM(yflux_lo),ARLIM(yflux_hi),
			                    xarea_dat,ARLIM(xarea_lo),ARLIM(xarea_hi),
			                    yarea_dat,ARLIM(yarea_lo),ARLIM(yarea_hi),
			                    dx,&mult);
#endif
#if (BL_SPACEDIM == 3)
		        const Box& zbx = Vsyncmfi.nodaltilebox(2);

            FArrayBox& zff = zflux[Vsyncmfi];
            DEF_LIMITS(zff,zflux_dat,zflux_lo,zflux_hi);

            const FArrayBox& zarea = area[2][Vsyncmfi];
            DEF_CLIMITS(zarea,zarea_dat,zarea_lo,zarea_hi);

            viscsyncflux (u_sync.dataPtr(comp), ARLIM(ulo), ARLIM(uhi),
			                    xbx.loVect(), xbx.hiVect(),
			                    ybx.loVect(), ybx.hiVect(),
			                    zbx.loVect(), zbx.hiVect(),
		                      xflux_dat,ARLIM(xflux_lo),ARLIM(xflux_hi),
			                    yflux_dat,ARLIM(yflux_lo),ARLIM(yflux_hi),
			                    zflux_dat,ARLIM(zflux_lo),ARLIM(zflux_hi),
			                    xarea_dat,ARLIM(xarea_lo),ARLIM(xarea_hi),
			                    yarea_dat,ARLIM(yarea_lo),ARLIM(yarea_hi),
			                    zarea_dat,ARLIM(zarea_lo),ARLIM(zarea_hi),
			                    dx,&mult);
#endif
	        }

	        if (update_fluxreg)
	        {
	          D_TERM(viscflux_reg->FineAdd(xflux,0,0,comp,1,1.0);,
		        viscflux_reg->FineAdd(yflux,1,0,comp,1,1.0);,
		        viscflux_reg->FineAdd(zflux,2,0,comp,1,1.0););
	        }
        }
    }
}
*/

void
Diffusion::diffuse_tensor_Vsync (MultiFab&              Vsync,
                                 Real                   dt,
                                 Real                   be_cn_theta,
                                 const MultiFab&        rho_half,
                                 int                    rho_flag,
                                 const MultiFab* const* beta,
                                 int                    betaComp,
				 bool                   update_fluxreg)
{
    BL_ASSERT(rho_flag == 1 || rho_flag == 3);

    if (verbose) amrex::Print() << "Diffusion::diffuse_tensor_Vsync ...\n";

    const MultiFab& volume = navier_stokes->Volume(); 
    const MultiFab* area   = navier_stokes->Area();
    // need for computeBeta. Don't see Why computeBeta defines area in this way
    // or why it even bothers to pass area when it's also passing geom
    const MultiFab *ap[AMREX_SPACEDIM];
    for (int d=0; d<AMREX_SPACEDIM; ++d)
    {
	ap[d] = &(area[d]);
    }

    MultiFab Rhs(grids,dmap,BL_SPACEDIM,0,MFInfo(),navier_stokes->Factory());

    MultiFab::Copy(Rhs,Vsync,0,0,BL_SPACEDIM,0);
    // SSync has mult by dt here. Needed here too?
    
    if (verbose > 1)
    {
        Real r_norm = Rhs.norm0();
	amrex::Print() << "Original max of Vsync " << r_norm << '\n';
    }
    //
    // Multiply RHS by volume and density.
    //
#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter Rhsmfi(Rhs,true); Rhsmfi.isValid(); ++Rhsmfi)
    {
	const Box&       bx   = Rhsmfi.tilebox();
        FArrayBox&       rhs  = Rhs[Rhsmfi];
        const FArrayBox& rho  = rho_half[Rhsmfi];
        const FArrayBox& prho = (navier_stokes->rho_ptime)[Rhsmfi];

        for (int comp = 0; comp < BL_SPACEDIM; comp++)
        {
	  // remove vol scaling
	  //rhs.mult(volume[Rhsmfi],bx,0,comp,1); 
            if (rho_flag == 1)
                rhs.mult(rho,bx,0,comp,1); 
            if (rho_flag == 3)
                rhs.mult(prho,bx,0,comp,1); 
        }
    }

    //
    // SET UP COEFFICIENTS FOR VISCOUS SOLVER.
    //
    const Real      a         = 1.0;
    const Real      b         = be_cn_theta*dt;
    const MultiFab& rho       = (rho_flag == 1) ? rho_half : navier_stokes->rho_ctime;
    Real rhsscale = 1.0;
	
    int soln_ng = 1;
    MultiFab Soln(grids,dmap,BL_SPACEDIM,soln_ng, MFInfo(),navier_stokes->Factory());
    Soln.setVal(0);

    //fixme
    // MultiFab Solnold(grids,dmap,BL_SPACEDIM,soln_ng, MFInfo(),navier_stokes->Factory());
    // Solnold.setVal(0);
    
    // MLMG
      const Real tol_rel = visc_tol;
      const Real tol_abs = -1;
      
      LPInfo info;
      info.setAgglomeration(agglomeration);
      info.setConsolidation(consolidation);
      //fixme??
      info.setMetricTerm(false);
      //info.setMaxCoarseningLevel(100);

#ifdef AMREX_USE_EB
      const auto& ebf = &dynamic_cast<EBFArrayBoxFactory const&>(navier_stokes->Factory());
      MLEBTensorOp tensorop({navier_stokes->Geom()}, {grids}, {dmap}, info, {ebf});
#else
      MLTensorOp tensorop({navier_stokes->Geom()}, {grids}, {dmap}, info);
#endif
      
      tensorop.setMaxOrder(tensor_max_order);

      // create right container
      Array<LinOpBCType,AMREX_SPACEDIM> mlmg_lobc[AMREX_SPACEDIM];
      Array<LinOpBCType,AMREX_SPACEDIM> mlmg_hibc[AMREX_SPACEDIM];
      // fill it
      for (int i=0; i<AMREX_SPACEDIM; i++)
	setDomainBC(mlmg_lobc[i], mlmg_hibc[i], Xvel+i);
      // pass to op
      tensorop.setDomainBC({AMREX_D_DECL(mlmg_lobc[0],mlmg_lobc[1],mlmg_lobc[2])},
			   {AMREX_D_DECL(mlmg_hibc[0],mlmg_hibc[1],mlmg_hibc[2])});

      // set up level BCs    
      if (level > 0) {
	tensorop.setCoarseFineBC(nullptr, crse_ratio[0]);
      }
      tensorop.setLevelBC(0, nullptr);
      
      {
	MultiFab acoef;
	// fixme ghostcells?
	acoef.define(grids, dmap, 1, 1, MFInfo(), navier_stokes->Factory());
	std::pair<Real,Real> scalars;
	// const Real cur_time = navier_stokes->get_state_data(State_Type).curTime();
	// computeAlpha(acoef, scalars, Xvel, a, b, cur_time, rho, rho_flag,
	// 	     &rhsscale, 0, nullptr);
	//FIXME - not sure about rho vs rho_half in computeAlpha
	// check for RZ 
	computeAlpha(acoef, scalars, a, b, rho,
		     rho_flag, &rhsscale, nullptr, 0, 
		     &rho, 0, 
		     navier_stokes->Geom(),volume,parent->Geom(0).IsRZ());

	tensorop.setScalars(scalars.first, scalars.second);
	tensorop.setACoeffs(0, acoef);
      }
      
      {
	Array<MultiFab,AMREX_SPACEDIM> face_bcoef;
	for (int n = 0; n < BL_SPACEDIM; n++)
	{
	  face_bcoef[n].define(area[n].boxArray(),area[n].DistributionMap(),1,0,MFInfo(),navier_stokes->Factory());
	}
	computeBeta(face_bcoef,nullptr,0,navier_stokes->Geom(),ap,
		    parent->Geom(0).IsRZ());
	//	computeBeta(face_bcoef, nullptr, 0);
	
	tensorop.setShearViscosity(0, amrex::GetArrOfConstPtrs(face_bcoef));
	// FIXME??? Hack 
	// remove the "divmusi" terms by setting kappa = (2/3) mu
	//  
	Print()<<"WARNING: Hack to get rid of divU terms ...\n";
	Array<MultiFab,AMREX_SPACEDIM> kappa;
	Real twothirds = 2.0/3.0;
	for (int idim = 0; idim < AMREX_SPACEDIM; ++idim)
	{
	  kappa[idim].define(face_bcoef[idim].boxArray(), face_bcoef[idim].DistributionMap(), 1, 0, MFInfo(),navier_stokes->Factory());
	  MultiFab::Copy(kappa[idim], face_bcoef[idim], 0, 0, 1, 0);
	  kappa[idim].mult(twothirds);
	}
	// bulk viscosity not ususally needed for gasses
	tensorop.setBulkViscosity(0, amrex::GetArrOfConstPtrs(kappa));

#ifdef AMREX_USE_EB
	MultiFab cc_bcoef(grids,dmap,BL_SPACEDIM,0,MFInfo(),navier_stokes->Factory());
	EB_average_face_to_cellcenter(cc_bcoef, 0,
				      amrex::GetArrOfConstPtrs(face_bcoef));
	tensorop.setEBShearViscosity(0, cc_bcoef);
	// part of hack to remove divmusi terms
	cc_bcoef.mult(twothirds);
	tensorop.setEBBulkViscosity(0, cc_bcoef);
#endif
      }

      MLMG mlmg(tensorop);
      //fixme?
      //mlmg.setMaxIter(max_iter);
      //mlmg.setBottomVerbose(bottom_verbose);
      if (use_hypre) {
	mlmg.setBottomSolver(MLMG::BottomSolver::hypre);
	mlmg.setBottomVerbose(hypre_verbose);
      }
      mlmg.setMaxFmgIter(max_fmg_iter);
      mlmg.setVerbose(verbose);

      // fixme? why bother with rhsscale since set = 1 above
      Rhs.mult(rhsscale,0,1);
      
      // ensures ghost cells of sol are correctly filled when returned from solver
      //fixme?? isn't FillPatch ususally the way to do it?
      // would need not not copy ghost cells to U_new below
      mlmg.setFinalFillBC(true);
      mlmg.solve({&Soln}, {&Rhs}, tol_rel, tol_abs);
    

#if 0
    {
    // Old Tensor Op 
    std::unique_ptr<DivVis> tensor_op ( getTensorOp(a,b,rho,beta,betaComp) );
    tensor_op->maxOrder(tensor_max_order);

    //
    // Construct solver and call it.
    //
    const Real S_tol     = visc_tol;
    const Real S_tol_abs = -1;
    if (use_tensor_cg_solve)
    {
        MCCGSolver cg(*tensor_op,use_mg_precond_flag);
        cg.solve(Solnold,Rhs,S_tol,S_tol_abs);
    }
    else
    {
        MCMultiGrid mg(*tensor_op);
        mg.solve(Solnold,Rhs,S_tol,S_tol_abs);
    }
    Rhs.clear();

    int visc_op_lev = 0;
    tensor_op->applyBC(Solnold,visc_op_lev); 
    }
#endif

    //fixme
    // compare solutions
    // static int count=0; count++;
    // MultiFab::Subtract(Solnold,Soln,0,0,AMREX_SPACEDIM,soln_ng);
    // amrex::WriteSingleLevelPlotfile("sdiff_"+std::to_string(count), Solnold, {AMREX_D_DECL("x","y","z")},navier_stokes->Geom(), 0.0, 0);
    /////
    
    //
    // Copy into state variable at new time.
    //
    MultiFab::Copy(Vsync,Soln,0,0,BL_SPACEDIM,soln_ng);
    
    if (verbose > 1)
    {
        Real s_norm = Soln.norm0(0,Soln.nGrow());
	amrex::Print() << "Final max of Vsync " << s_norm << '\n';
    }

    if (level > 0)
    {
        FluxBoxes fb(navier_stokes, BL_SPACEDIM);
        MultiFab** tensorflux = fb.get();

	// MultiFab** tensorflux_old = new MultiFab*[BL_SPACEDIM];
	// for (int dir = 0; dir < BL_SPACEDIM; dir++)
	// {
	//   const BoxArray& ba = navier_stokes->getEdgeBoxArray(dir);
	//   const DistributionMapping& dm = navier_stokes->DistributionMap();
	//   tensorflux_old[dir] = new MultiFab(ba,dm,AMREX_SPACEDIM,0);
	// }

	std::array<MultiFab*,AMREX_SPACEDIM> fp{AMREX_D_DECL(tensorflux[0], tensorflux[1], tensorflux[2])};

	mlmg.getFluxes({fp},{&Soln});

#if 0
        // old way      
        tensor_op->compFlux(D_DECL(*(tensorflux[0]), *(tensorflux[1]), *(tensorflux[2])),Soln);
#endif

	// FIXME update these comments...
        // The extra factor of dt comes from the fact that Vsync looks
        // like dV/dt, not just an increment to V.
        //
        // This is to remove the dx scaling in the coeffs
        //
	int flux_ng = tensorflux[0]->nGrow();
        for (int d =0; d <BL_SPACEDIM; d++){
	  //tensorflux[d]->mult(b/(dt*navier_stokes->Geom().CellSize()[d]),0);
	  for (int i = 0; i < BL_SPACEDIM; ++i){
	    // we've done away with vol weighting in A,B
	    MultiFab::Multiply(*tensorflux[d],area[d],0,i,1,flux_ng);
	  }
	  tensorflux[d]->mult(b/dt,0,AMREX_SPACEDIM,flux_ng);
	}

	if (update_fluxreg)
	{	  
	  for (int k = 0; k < BL_SPACEDIM; k++)
	    viscflux_reg->FineAdd(*(tensorflux[k]),k,Xvel,Xvel,
	  			  BL_SPACEDIM,dt*dt);
	}
    }
}

void
Diffusion::diffuse_Ssync (MultiFab&              Ssync,
                          int                    sigma,
                          Real                   dt,
                          Real                   be_cn_theta,
                          const MultiFab&        rho_half,
                          int                    rho_flag,
                          MultiFab* const*       flux,
                          int                    fluxComp,
                          const MultiFab* const* beta,
                          int                    betaComp,
                          const MultiFab*        alpha,
                          int                    alphaComp)
{
    const MultiFab& volume = navier_stokes->Volume(); 
    const int state_ind    = sigma + BL_SPACEDIM;
    //
    // Fixme!! this solve still has volume scaling...
    //
    if (verbose)
    {
        amrex::Print() << "Diffusion::diffuse_Ssync lev: " << level << " "
                       << navier_stokes->get_desc_lst()[State_Type].name(state_ind) << '\n';
    }

    const Real strt_time = ParallelDescriptor::second();

    int allnull, allthere;
    checkBeta(beta, allthere, allnull);

    MultiFab  Rhs(grids,dmap,1,0,MFInfo(),navier_stokes->Factory());

    MultiFab::Copy(Rhs,Ssync,sigma,0,1,0);

    Rhs.mult(dt);


    if (verbose > 1)
    {
        MultiFab junk(grids,dmap,1,0,MFInfo(),navier_stokes->Factory());

        MultiFab::Copy(junk,Rhs,0,0,1,0);

        if (rho_flag == 2)
        {
            MultiFab& S_new = navier_stokes->get_new_data(State_Type);
	    MultiFab::Divide(junk, S_new, Density, 0, 1, 0);
        }
        Real r_norm = junk.norm0();
	amrex::Print() << "Original max of Ssync " << r_norm << '\n';
    }
    //
    // SET UP COEFFICIENTS FOR VISCOUS SOLVER.
    //
    const Real a = 1.0;
    Real       b = be_cn_theta*dt;
    if (allnull)
        b *= visc_coef[state_ind];
    Real           rhsscale = 1.0;

    const Real S_tol     = visc_tol;
    const Real S_tol_abs = -1;

    MultiFab Soln(grids,dmap,1,1,MFInfo(),navier_stokes->Factory());
    Soln.setVal(0);

    LPInfo info;
    info.setAgglomeration(agglomeration);
    info.setConsolidation(consolidation);
    info.setMetricTerm(false);
       
#ifdef AMREX_USE_EB
      const auto& ebf = &dynamic_cast<EBFArrayBoxFactory const&>(navier_stokes->Factory());
      MLEBABecLap mlabec({navier_stokes->Geom()}, {grids}, {dmap}, info, {ebf});
#else
    MLABecLaplacian mlabec({navier_stokes->Geom()}, {grids}, {dmap}, info);
#endif
    mlabec.setMaxOrder(max_order);

    std::array<LinOpBCType,AMREX_SPACEDIM> mlmg_lobc;
    std::array<LinOpBCType,AMREX_SPACEDIM> mlmg_hibc;
    setDomainBC(mlmg_lobc, mlmg_hibc, state_ind);
       
    mlabec.setDomainBC(mlmg_lobc, mlmg_hibc);
    if (level > 0) {
      mlabec.setCoarseFineBC(nullptr, crse_ratio[0]);
    }
    mlabec.setLevelBC(0, nullptr);

    {
      MultiFab acoef;
      std::pair<Real,Real> scalars;
      const Real cur_time = navier_stokes->get_state_data(State_Type).curTime();
      computeAlpha(acoef, scalars, state_ind, a, b, cur_time, rho_half, rho_flag,
                   &rhsscale, alphaComp, alpha);
      mlabec.setScalars(scalars.first, scalars.second);
      mlabec.setACoeffs(0, acoef);
    }
        
    {
      std::array<MultiFab,BL_SPACEDIM> bcoeffs;
      computeBeta(bcoeffs, beta, betaComp);
      mlabec.setBCoeffs(0, amrex::GetArrOfConstPtrs(bcoeffs));
    }

    MLMG mlmg(mlabec);
    if (use_hypre) {
      mlmg.setBottomSolver(MLMG::BottomSolver::hypre);
      mlmg.setBottomVerbose(hypre_verbose);
    }
    mlmg.setMaxFmgIter(max_fmg_iter);
    mlmg.setVerbose(verbose);

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter Rhsmfi(Rhs,true); Rhsmfi.isValid(); ++Rhsmfi)
    {
      const Box& bx = Rhsmfi.tilebox();
      Rhs[Rhsmfi].mult(volume[Rhsmfi],bx,0,0); 
      if (rho_flag == 1) {
        Rhs[Rhsmfi].mult(rho_half[Rhsmfi],bx,0,0);
      }
      Rhs[Rhsmfi].mult(rhsscale,bx);
    }

    mlmg.solve({&Soln}, {&Rhs}, S_tol, S_tol_abs);
        
    int flux_allthere, flux_allnull;
    checkBeta(flux, flux_allthere, flux_allnull);
    if (flux_allthere)
    {
      AMREX_D_TERM(MultiFab flxx(*flux[0], amrex::make_alias, fluxComp, 1);,
                   MultiFab flxy(*flux[1], amrex::make_alias, fluxComp, 1);,
                   MultiFab flxz(*flux[2], amrex::make_alias, fluxComp, 1););
                   std::array<MultiFab*,AMREX_SPACEDIM> fp{AMREX_D_DECL(&flxx,&flxy,&flxz)};
                   mlmg.getFluxes({fp});
      for (int i = 0; i < BL_SPACEDIM; ++i) {
        (*flux[i]).mult(b/(dt*navier_stokes->Geom().CellSize()[i]),fluxComp,1,0);
       }
    }

    MultiFab::Copy(Ssync,Soln,0,sigma,1,0);
    
    if (verbose > 1)
    {
        Real s_norm = Soln.norm0(0,Soln.nGrow());
	      amrex::Print() << "Final max of Ssync " << s_norm << '\n';
    }
    
    if (rho_flag == 2)
    {
        MultiFab& S_new = navier_stokes->get_new_data(State_Type);

#ifdef _OPENMP
#pragma omp parallel
#endif
        for (MFIter Ssyncmfi(Ssync,true); Ssyncmfi.isValid(); ++Ssyncmfi)
        {
            Ssync[Ssyncmfi].mult(S_new[Ssyncmfi],Ssyncmfi.tilebox(),Density,sigma,1);
        }
    }

    if (verbose)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

	amrex::Print() << "Diffusion::diffuse_Ssync(): lev: " << level
		       << ", time: " << run_time << '\n';
    }
}

void
Diffusion::getTensorOp_doit (DivVis*                tensor_op,
                             Real                   a,
                             Real                   b,
                             const MultiFab&        rho,
                             const MultiFab* const* beta,
                             int                    betaComp)
{
    const MultiFab& volume = navier_stokes->Volume(); 
    const MultiFab* area   = navier_stokes->Area(); 

    int allthere;
    checkBeta(beta, allthere);

    int       isrz       = parent->Geom(0).IsRZ();
    const int nghost     = 1;
    const int nCompAlpha = BL_SPACEDIM == 2  ?  2  :  1;

    const Real* dx = navier_stokes->Geom().CellSize();

    MultiFab alpha(grids,dmap,nCompAlpha,nghost);

    alpha.setVal(0,nghost);

    if (a != 0.0)
    {
#ifdef _OPENMP
#pragma omp parallel
#endif
        for (MFIter mfi(alpha,true); mfi.isValid(); ++mfi)
        {
            const Box&  bx        = mfi.tilebox();
            Vector<Real> rcen(bx.length(0));

            navier_stokes->Geom().GetCellLoc(rcen, bx, 0);

            const int*  lo        = bx.loVect();
            const int*  hi        = bx.hiVect();
            Real*       alpha_dat = alpha[mfi].dataPtr();
            Box         abx       = alpha[mfi].box();
            const int*  alo       = abx.loVect();
            const int*  ahi       = abx.hiVect();
            const Real* rcendat   = rcen.dataPtr();
            const Real* voli      = volume[mfi].dataPtr();
            const Box&  vbox      = volume[mfi].box();
            const int*  vlo       = vbox.loVect();
            const int*  vhi       = vbox.hiVect();

            const FArrayBox& Rh = rho[mfi];
            DEF_CLIMITS(Rh,rho_dat,rlo,rhi);

            const FArrayBox&  betax = (*beta[0])[mfi];
            const Real* betax_dat   = betax.dataPtr(betaComp);
            const int*  betax_lo    = betax.loVect();
            const int*  betax_hi    = betax.hiVect();

            const FArrayBox&  betay = (*beta[1])[mfi];
            const Real* betay_dat   = betay.dataPtr(betaComp);
            const int*  betay_lo    = betay.loVect();
            const int*  betay_hi    = betay.hiVect();

#if (BL_SPACEDIM == 3)
            const FArrayBox&  betaz = (*beta[2])[mfi];
            const Real* betaz_dat   = betaz.dataPtr(betaComp);
            const int*  betaz_lo    = betaz.loVect();
            const int*  betaz_hi    = betaz.hiVect();
#endif

            set_tensor_alpha(alpha_dat, ARLIM(alo), ARLIM(ahi),
			     lo, hi, rcendat, ARLIM(lo), ARLIM(hi), &b,
			     voli, ARLIM(vlo), ARLIM(vhi),
			     rho_dat,ARLIM(rlo),ARLIM(rhi),
			     betax_dat,ARLIM(betax_lo),ARLIM(betax_hi),
			     betay_dat,ARLIM(betay_lo),ARLIM(betay_hi),
#if (BL_SPACEDIM == 3)
			     betaz_dat,ARLIM(betaz_lo),ARLIM(betaz_hi),
#endif
			     &isrz);
        }
    }
    tensor_op->setScalars(a,b);
    tensor_op->aCoefficients(alpha);

    alpha.clear();

    for (int n = 0; n < BL_SPACEDIM; n++)
    {
        MultiFab bcoeffs(area[n].boxArray(),area[n].DistributionMap(),1,0);
	
#ifdef _OPENMP
#pragma omp parallel
#endif
	for (MFIter bcoeffsmfi(*beta[n],true); bcoeffsmfi.isValid(); ++bcoeffsmfi)
	{
	    const Box& bx = bcoeffsmfi.tilebox();
	      
	    bcoeffs[bcoeffsmfi].copy(area[n][bcoeffsmfi],bx,0,bx,0,1);
	    bcoeffs[bcoeffsmfi].mult(dx[n],bx);
	    bcoeffs[bcoeffsmfi].mult((*beta[n])[bcoeffsmfi],bx,bx,betaComp,0,1);
	}
	
	tensor_op->bCoefficients(bcoeffs,n); // not thread safe?
    }
}

DivVis*
Diffusion::getTensorOp (Real                   a,
                        Real                   b,
                        Real                   time,
                        ViscBndryTensor&       visc_bndry,
                        const MultiFab&        rho,
                        const MultiFab* const* beta,
                        int                    betaComp)
{
    const Real* dx = navier_stokes->Geom().CellSize();

    getTensorBndryData(visc_bndry,time);

    DivVis* tensor_op = new DivVis(visc_bndry,dx);

    tensor_op->maxOrder(tensor_max_order);

    getTensorOp_doit(tensor_op, a, b, rho, beta, betaComp);

    return tensor_op;
}

DivVis*
Diffusion::getTensorOp (Real                   a,
                        Real                   b,
                        const MultiFab&        rho,
                        const MultiFab* const* beta,
                        int                    betaComp)
{
    int allthere;
    checkBeta(beta, allthere);

    const Real* dx   = navier_stokes->Geom().CellSize();
    const int   nDer = MCLinOp::bcComponentsNeeded();

    Vector<BCRec> bcarray(nDer,BCRec(D_DECL(EXT_DIR,EXT_DIR,EXT_DIR),
                                    D_DECL(EXT_DIR,EXT_DIR,EXT_DIR)));

    for (int id = 0; id < BL_SPACEDIM; id++)
    {
        bcarray[id] = navier_stokes->get_desc_lst()[State_Type].getBC(Xvel+id);
    }

    IntVect ref_ratio = level > 0 ? parent->refRatio(level-1) : IntVect::TheUnitVector();

    ViscBndryTensor bndry;

    bndry.define(grids,dmap,nDer,navier_stokes->Geom());
    bndry.setHomogValues(bcarray, ref_ratio[0]);

    DivVis* tensor_op = new DivVis(bndry,dx);

    tensor_op->maxOrder(tensor_max_order);

    getTensorOp_doit(tensor_op, a, b, rho, beta, betaComp);

    return tensor_op;
}

ABecLaplacian*
Diffusion::getViscOp (int                    comp,
                      Real                   a,
                      Real                   b,
                      Real                   time,
                      ViscBndry&             visc_bndry,
                      const MultiFab&        rho_half,
                      int                    rho_flag, 
                      Real*                  rhsscale,
                      const MultiFab* const* beta,
                      int                    betaComp,
                      const MultiFab*        alpha_in,
                      int                    alphaComp,
                      bool		     bndry_already_filled)
{
    const Real* dx = navier_stokes->Geom().CellSize();

    if (!bndry_already_filled)
        getBndryData(visc_bndry,comp,1,time,rho_flag);

    ABecLaplacian* visc_op = new ABecLaplacian(visc_bndry,dx);

    visc_op->maxOrder(max_order);

    setAlpha(visc_op,comp,a,b,time,rho_half,rho_flag,rhsscale,alphaComp,alpha_in);

    setBeta(visc_op,beta,betaComp);

    return visc_op;
}

void
Diffusion::getBndryDataGivenS (ViscBndry&               bndry,
                               const Vector<MultiFab*>& S,
                               int                      S_comp,
                               const Vector<MultiFab*>& Rho,
                               int                      Rho_comp,
                               const BCRec&             bc,
                               const IntVect&           crat,
                               int                      rho_flag)
{
  const int nGrow = 1;
  const int nComp = 1;

  MultiFab tmp(S[0]->boxArray(),S[0]->DistributionMap(),nComp,nGrow);

  MultiFab::Copy(tmp,*S[0],S_comp,0,1,nGrow);
  if (rho_flag == 2)
      MultiFab::Divide(tmp,*Rho[0],Rho_comp,0,1,nGrow);

  bool has_coarse_data = S.size() > 1;
  if (!has_coarse_data)
  {
    bndry.setBndryValues(tmp,0,0,nComp,bc);
  }
  else
  {
    BoxArray cgrids = S[0]->boxArray();
    cgrids.coarsen(crat);
    BndryRegister crse_br(cgrids,S[0]->DistributionMap(),0,1,2,nComp);
    //
    // interp for solvers over ALL c-f brs, need safe data.
    //
    crse_br.setVal(BL_BOGUS);
    MultiFab tmpc(S[1]->boxArray(),S[1]->DistributionMap(),nComp,nGrow);
    MultiFab::Copy(tmpc,*S[1],S_comp,0,1,nGrow);
    if (rho_flag == 2)
        MultiFab::Divide(tmpc,*Rho[1],Rho_comp,0,1,nGrow);
    crse_br.copyFrom(tmpc,nGrow,0,0,nComp);
    bndry.setBndryValues(crse_br,0,tmp,0,0,nComp,crat,bc);
  }
}

ABecLaplacian*
Diffusion::getViscOp (Real                                 a,
                      Real                                 b,
                      ViscBndry&                           visc_bndry,
                      const Vector<MultiFab*>&             S,
                      int                                  S_comp,
                      const Vector<MultiFab*>&             Rho,
                      int                                  Rho_comp,
                      const MultiFab&                      rho_half,
                      int                                  rho_flag, 
                      Real*                                rhsscale,
                      const MultiFab* const*               beta,
                      int                                  betaComp,
                      const MultiFab*                      alpha_in,
                      int                                  alpha_in_comp,
                      MultiFab&                            alpha,
                      std::array<MultiFab,AMREX_SPACEDIM>& bcoeffs,
                      const BCRec&                         bc,
                      const IntVect&                       crat,
                      const Geometry&                      geom,
                      const MultiFab&                      volume,
                      const MultiFab* const*               area,
                      bool                                 use_hoop_stress)
{
    getBndryDataGivenS(visc_bndry,S,S_comp,Rho,Rho_comp,bc,crat,rho_flag);

    ABecLaplacian* visc_op = new ABecLaplacian(visc_bndry,geom.CellSize());

    visc_op->maxOrder(max_order);

    setAlpha(visc_op,a,b,rho_half,rho_flag,rhsscale,alpha_in,alpha_in_comp,
             Rho[0],Rho_comp,alpha,geom,volume,use_hoop_stress);

    setBeta(visc_op,beta,betaComp,bcoeffs,geom,area,use_hoop_stress);

    return visc_op;
}

ABecLaplacian*
Diffusion::getViscOp (int                    comp,
                      Real                   a,
                      Real                   b,
                      const MultiFab&        rho,
                      int                    rho_flag,
                      Real*                  rhsscale,
                      const MultiFab* const* beta,
                      int                    betaComp,
                      const MultiFab*        alpha_in,
                      int                    alphaComp)
{
    //
    // Note: This assumes that the "NEW" density is to be used, if rho_flag==2
    //
    const Geometry& geom = navier_stokes->Geom();
    const Real*  dx      = geom.CellSize();
    const BCRec& bc      = navier_stokes->get_desc_lst()[State_Type].getBC(comp);

    IntVect ref_ratio = level > 0 ? parent->refRatio(level-1) : IntVect::TheUnitVector();

    ViscBndry bndry(grids,dmap,1,geom);
    bndry.setHomogValues(bc, ref_ratio);

    ABecLaplacian* visc_op = new ABecLaplacian(bndry,dx);
    visc_op->maxOrder(max_order);

    const Real time = navier_stokes->get_state_data(State_Type).curTime();

    setAlpha(visc_op,comp,a,b,time,rho,rho_flag,rhsscale,alphaComp,alpha_in);

    setBeta(visc_op,beta,betaComp);

    return visc_op;
}

void
Diffusion::setAlpha (ABecLaplacian*  visc_op,
                     int             comp,
                     Real            a,
                     Real            b,
                     Real            time,
                     const MultiFab& rho,
                     int             rho_flag, 
                     Real*           rhsscale,
                     int             dataComp,
                     const MultiFab* alpha_in)
{
    BL_ASSERT(visc_op != 0);

    MultiFab alpha;
    std::pair<Real,Real> scalars;
    computeAlpha(alpha, scalars, comp, a, b, time, rho, rho_flag, rhsscale, dataComp, alpha_in);

    visc_op->setScalars(scalars.first, scalars.second);
    visc_op->aCoefficients(alpha);
}

void
Diffusion::setAlpha (ABecLaplacian*  visc_op,
                     Real            a,
                     Real            b,
                     const MultiFab& rho_half,
                     int             rho_flag, 
                     Real*           rhsscale,
                     const MultiFab* alpha_in,
                     int             alpha_in_comp,
                     const MultiFab* rho,
                     int             rho_comp,
                     MultiFab&       alpha,
                     const Geometry& geom,
                     const MultiFab& volume,
                     bool            use_hoop_stress)
{
    BL_ASSERT(visc_op != 0);

    std::pair<Real,Real> scalars;
    computeAlpha(alpha, scalars, a, b, rho_half, rho_flag, rhsscale, alpha_in, alpha_in_comp,
                 rho, rho_comp, geom, volume, use_hoop_stress);

    visc_op->setScalars(scalars.first, scalars.second);

    visc_op->aCoefficients(alpha);
}

void
Diffusion::computeAlpha (MultiFab&       alpha,
                         std::pair<Real,Real>& scalars,
                         int             comp,
                         Real            a,
                         Real            b,
                         Real            time,
                         const MultiFab& rho,
                         int             rho_flag, 
                         Real*           rhsscale,
                         int             dataComp,
                         const MultiFab* alpha_in)
{
/*
#if 0
  //#ifdef AMREX_USE_EB
  //fixme? do we want to assume everything passed in has good data in enough ghost cells
  //  or do we want take ng=0 and then fill alpha's ghost cells after?
  //    int ng = eb_ngrow;
  // Don't think alpha needs any grow cells... see ng comments in diffuse scalar
    int ng = 0;
    alpha.define(grids, dmap, 1, ng, MFInfo(), navier_stokes->Factory());
      
    if (alpha_in != 0){
        BL_ASSERT(dataComp >= 0 && dataComp < alpha.nComp());
	// fixme? again original did not use any ghost cells
	MultiFab::Copy(alpha,*alpha_in,dataComp,0,1,ng);
    }
    else{
      alpha.setVal(1.0);
    }

    if ( rho_flag == 1 ) {
      MultiFab::Multiply(alpha,rho,0,0,1,ng);
    }
    else if (rho_flag == 2 || rho_flag == 3) {
      MultiFab& S = navier_stokes->get_data(State_Type,time);
      // original didn't copy any ghost cells...
      MultiFab::Multiply(alpha,S,Density,0,1,ng);
    }
  
#else
*/

    int ng = 1;

    alpha.define(grids, dmap, 1, ng, MFInfo(), navier_stokes->Factory());

    const MultiFab& volume = navier_stokes->Volume(); 

    int usehoop = (comp == Xvel && (parent->Geom(0).IsRZ()));
    int useden  = (rho_flag == 1);

    if (!usehoop)
    {
      //MultiFab::Copy(alpha, volume, 0, 0, 1, ng);
	alpha.setVal(1.0); // Here we reset to 1. to remove the volume scaling
	// Warning this will fail in Sync routines, but we will see that later

        if (useden) 
            MultiFab::Multiply(alpha,rho,0,0,1,ng);
    }
    else
    {
#ifdef _OPENMP
#pragma omp parallel
#endif
        for (MFIter mfi(alpha,true); mfi.isValid(); ++mfi)
        {
            const Box& bx = mfi.tilebox();

            Vector<Real> rcen(bx.length(0));
            navier_stokes->Geom().GetCellLoc(rcen, bx, 0);

            const int*       lo      = bx.loVect();
            const int*       hi      = bx.hiVect();
            Real*            dat     = alpha[mfi].dataPtr();
            const Box&       abx     = alpha[mfi].box();
            const int*       alo     = abx.loVect();
            const int*       ahi     = abx.hiVect();
            const Real*      rcendat = rcen.dataPtr();
            const Real*      voli    = volume[mfi].dataPtr();
            const Box&       vbox    = volume[mfi].box();
            const int*       vlo     = vbox.loVect();
            const int*       vhi     = vbox.hiVect();
            const FArrayBox& Rh      = rho[mfi];

            DEF_CLIMITS(Rh,rho_dat,rlo,rhi);

            fort_setalpha(dat, ARLIM(alo), ARLIM(ahi),
                          lo, hi, rcendat, ARLIM(lo), ARLIM(hi), &b,
                          voli, ARLIM(vlo), ARLIM(vhi),
                          rho_dat,ARLIM(rlo),ARLIM(rhi),&usehoop,&useden);
        }
    }

    if (rho_flag == 2 || rho_flag == 3)
    {
        MultiFab& S = navier_stokes->get_data(State_Type,time);

#ifdef _OPENMP
#pragma omp parallel
#endif
	for (MFIter alphamfi(alpha,true); alphamfi.isValid(); ++alphamfi)
        {
	  BL_ASSERT(grids[alphamfi.index()].contains(alphamfi.tilebox())==1);
	    alpha[alphamfi].mult(S[alphamfi],alphamfi.tilebox(),Density,0,1);
        }
    }

    if (alpha_in != 0)
    {
        BL_ASSERT(dataComp >= 0 && dataComp < alpha.nComp());

#ifdef _OPENMP
#pragma omp parallel
#endif
        for (MFIter alphamfi(alpha,true); alphamfi.isValid(); ++alphamfi)
        {
            alpha[alphamfi].mult((*alpha_in)[alphamfi],alphamfi.tilebox(),dataComp,0,1);
        }
    }

    if (rhsscale != 0)
    {
        *rhsscale = scale_abec ? 1.0/alpha.max(0) : 1.0;

        scalars.first = a*(*rhsscale);
        scalars.second = b*(*rhsscale);
    }
    else
    {
        scalars.first = a;
        scalars.second = b;
    }
}

void
Diffusion::computeAlpha (MultiFab&       alpha,
                         std::pair<Real,Real>& scalars,
                         Real            a,
                         Real            b,
                         const MultiFab& rho_half,
                         int             rho_flag, 
                         Real*           rhsscale,
                         const MultiFab* alpha_in,
                         int             alpha_in_comp,
                         const MultiFab* rho,
                         int             rho_comp,
                         const Geometry& geom,
                         const MultiFab& volume,
                         bool            use_hoop_stress)
{

    int useden  = (rho_flag == 1);

    if (!use_hoop_stress)
    {
      alpha.setVal(1.0); // Here we reset to 1. to remove the volume scaling
      // Is this (copied from above computeAlpha) applicable?
      // Warning this will fail in Sync routines, but we will see that later
      if (useden) 
	MultiFab::Multiply(alpha,rho_half,0,0,1,0);
    }
    else
    {

#ifdef _OPENMP
#pragma omp parallel
#endif
        for (MFIter mfi(alpha,true); mfi.isValid(); ++mfi)
        {

            const Box& bx = mfi.tilebox();

            Vector<Real> rcen(bx.length(0));
            geom.GetCellLoc(rcen, bx, 0);

            const int*       lo      = bx.loVect();
            const int*       hi      = bx.hiVect();
            Real*            dat     = alpha[mfi].dataPtr();
            const Box&       abx     = alpha[mfi].box();
            const int*       alo     = abx.loVect();
            const int*       ahi     = abx.hiVect();
            const Real*      rcendat = rcen.dataPtr();
            const Real*      voli    = volume[mfi].dataPtr();
            const Box&       vbox    = volume[mfi].box();
            const int*       vlo     = vbox.loVect();
            const int*       vhi     = vbox.hiVect();
	    // fixme: Not sure which rho we want here. 
            const FArrayBox& Rh      = rho_half[mfi];
            int usehoop              = (int)use_hoop_stress;

            DEF_CLIMITS(Rh,rho_dat,rlo,rhi);

            fort_setalpha(dat, ARLIM(alo), ARLIM(ahi),
                          lo, hi, rcendat, ARLIM(lo), ARLIM(hi), &b,
                          voli, ARLIM(vlo), ARLIM(vhi),
                          rho_dat,ARLIM(rlo),ARLIM(rhi),&usehoop,&useden);
        }
    }

    if (rho_flag == 2 || rho_flag == 3)
    {
        MultiFab::Multiply(alpha,*rho,rho_comp,0,1,0);
    }

    if (alpha_in != 0)
    {
        BL_ASSERT(alpha_in_comp >= 0 && alpha_in_comp < alpha.nComp());
        MultiFab::Multiply(alpha,*alpha_in,alpha_in_comp,0,1,0);
    }

    if (rhsscale != 0)
    {
        *rhsscale = scale_abec ? 1.0/alpha.max(0) : 1.0;

        scalars.first = a*(*rhsscale);
        scalars.second = b*(*rhsscale);
    }
    else
    {
        scalars.first = a;
        scalars.second = b;
    }
}

void
Diffusion::setBeta (ABecLaplacian*         visc_op,
                    const MultiFab* const* beta,
                    int                    betaComp)
{
    BL_ASSERT(visc_op != 0);

    std::array<MultiFab,AMREX_SPACEDIM> bcoeffs;

    computeBeta(bcoeffs, beta, betaComp);

    for (int n = 0; n < AMREX_SPACEDIM; n++)
    {
        visc_op->bCoefficients(bcoeffs[n],n);
    }
}

void
Diffusion::setBeta (ABecLaplacian*         visc_op,
                    const MultiFab* const* beta,
                    int                    betaComp,
                    std::array<MultiFab,AMREX_SPACEDIM>& bcoeffs,
                    const Geometry&        geom,
                    const MultiFab* const* area,
                    bool            use_hoop_stress)
{
    BL_ASSERT(visc_op != 0);

    computeBeta(bcoeffs, beta, betaComp, geom, area, use_hoop_stress);

    for (int n = 0; n < AMREX_SPACEDIM; n++)
    {
        visc_op->bCoefficients(bcoeffs[n],n);
    }
}

void
Diffusion::computeBeta (std::array<MultiFab,AMREX_SPACEDIM>& bcoeffs,
                        const MultiFab* const* beta,
                        int                    betaComp)
{
    const MultiFab* area = navier_stokes->Area(); 

    for (int n = 0; n < BL_SPACEDIM; n++)
    {
	bcoeffs[n].define(area[n].boxArray(),area[n].DistributionMap(),1,0);
    }

    int allnull, allthere;
    checkBeta(beta, allthere, allnull);

    const Real* dx = navier_stokes->Geom().CellSize();

    if (allnull)
    {
        for (int n = 0; n < BL_SPACEDIM; n++)
        {
	    MultiFab::Copy(bcoeffs[n], area[n], 0, 0, 1, 0);
	    bcoeffs[n].mult(dx[n]);
        }
    }
    else
    {
#ifdef _OPENMP
#pragma omp parallel
#endif
        for (int n = 0; n < BL_SPACEDIM; n++)
        {
	    for (MFIter bcoeffsmfi(*beta[n],true); bcoeffsmfi.isValid(); ++bcoeffsmfi)
            {
 	        const Box& bx = bcoeffsmfi.tilebox();
	      
 		bcoeffs[n][bcoeffsmfi].copy(area[n][bcoeffsmfi],bx,0,bx,0,1);
		bcoeffs[n][bcoeffsmfi].mult((*beta[n])[bcoeffsmfi],bx,bx,betaComp,0,1);
		bcoeffs[n][bcoeffsmfi].mult(dx[n],bx);
            }
        }
    }
}

void
Diffusion::computeBeta (std::array<MultiFab,AMREX_SPACEDIM>& bcoeffs,
                        const MultiFab* const* beta,
                        int                    betaComp,
                        const Geometry&        geom,
                        const MultiFab* const* area,
                        bool                   use_hoop_stress)
{ 
    int allnull, allthere;
    checkBeta(beta, allthere, allnull);

    const Real* dx = geom.CellSize();

    if (allnull)
    {
      for (int n = 0; n < BL_SPACEDIM; n++)
      {
        if (use_hoop_stress){
	  MultiFab::Copy(bcoeffs[n], *area[n], 0, 0, 1, 0);
	  bcoeffs[n].mult(dx[n]);
        }
        else
        {      
          bcoeffs[n].setVal(1.0);
        }
      }
    }
    else
    {

#ifdef _OPENMP
#pragma omp parallel
#endif
      for (int n = 0; n < BL_SPACEDIM; n++)
      {
	for (MFIter bcoeffsmfi(*beta[n],true); bcoeffsmfi.isValid(); ++bcoeffsmfi)
	{
	  const Box& bx = bcoeffsmfi.tilebox();
    
          if (use_hoop_stress){
	    bcoeffs[n][bcoeffsmfi].copy((*area[n])[bcoeffsmfi],bx,0,bx,0,1);
            bcoeffs[n][bcoeffsmfi].mult((*beta[n])[bcoeffsmfi],bx,bx,betaComp,0,1);
	    bcoeffs[n][bcoeffsmfi].mult(dx[n],bx);
          }
          else
	  {
            bcoeffs[n][bcoeffsmfi].setVal(1.0,bx);
	    bcoeffs[n][bcoeffsmfi].mult((*beta[n])[bcoeffsmfi],bx,bx,betaComp,0,1);
          }
        }
      }
    }
}

void
Diffusion::getViscTerms (MultiFab&              visc_terms,
                         int                    src_comp,
                         int                    comp, 
                         Real                   time,
                         int                    rho_flag,
                         const MultiFab* const* beta,
                         int                    betaComp)
{
    int allnull, allthere;
    checkBeta(beta, allthere, allnull);
    //
    // Before computing the godunov predictors we may have to
    // precompute the viscous source terms.  To do this we must
    // construct a Laplacian operator, set the coeficients and apply
    // it to the time N data.  First, however, we must precompute the
    // fine N bndry values.  We will do this for each scalar that diffuses.
    //
    // Note: This routine DOES NOT fill grow cells
    //

    //
    // FIXME
    // LinOp classes cannot handle multcomponent MultiFabs yet,
    // construct the components one at a time and copy to visc_terms.
    //
#if 0
    // old way with volume weighted beta
    MultiFab&   S  = navier_stokes->get_data(State_Type,time);
    const Real* dx = navier_stokes->Geom().CellSize();

    if (is_diffusive[comp])
    {
        MultiFab visc_tmp(grids,dmap,1,1), s_tmp(grids,dmap,1,1);

        ViscBndry visc_bndry;
        getBndryData(visc_bndry,comp,1,time,rho_flag);
        //
        // Set up operator and apply to compute viscous terms.
        //
        const Real a = 0.0;
        const Real b = allnull ? -visc_coef[comp] : -1.0;

        ABecLaplacian visc_op(visc_bndry,dx);

        visc_op.setScalars(a,b);
        visc_op.maxOrder(max_order);

	// setBeta still puts in vol factor???
        setBeta(&visc_op,beta,betaComp);
        //
        // Copy to single component multifab for operator classes.
        //
        MultiFab::Copy(s_tmp,S,comp,0,1,0);

        if (rho_flag == 2)
        {
            //
            // We want to evaluate (div beta grad) S, not rho*S.
            //
	    MultiFab::Divide(s_tmp, S, Density, 0, 1, 0);
        }

        visc_op.apply(visc_tmp,s_tmp);
        //
        // Must divide by volume.
        //
        {
	    const MultiFab& volume = navier_stokes->Volume(); 
	    MultiFab::Divide(visc_tmp, volume, 0, 0, 1, 0);
        }

#if (BL_SPACEDIM == 2)
        if (comp == Xvel && parent->Geom(0).IsRZ())
        {
#ifdef _OPENMP
#pragma omp parallel
#endif
	  for (MFIter visc_tmpmfi(visc_tmp,true); visc_tmpmfi.isValid(); ++visc_tmpmfi)
            {
                //
                // visc_tmp[k] += -mu * u / r^2
                //
                const int  i   = visc_tmpmfi.index();
                const Box& bx  = visc_tmpmfi.tilebox();
		const Box& tmpbx = visc_tmpmfi.validbox();

		Box        vbx = amrex::grow(tmpbx,visc_tmp.nGrow());
                Box        sbx = amrex::grow(s_tmp.box(i),s_tmp.nGrow());
                Vector<Real> rcen(bx.length(0));
                navier_stokes->Geom().GetCellLoc(rcen, bx, 0);
                const int*  lo      = bx.loVect();
                const int*  hi      = bx.hiVect();
                const int*  vlo     = vbx.loVect();
                const int*  vhi     = vbx.hiVect();
                const int*  slo     = sbx.loVect();
                const int*  shi     = sbx.hiVect();
                Real*       vdat    = visc_tmp[visc_tmpmfi].dataPtr();
                Real*       sdat    = s_tmp[visc_tmpmfi].dataPtr();
                const Real* rcendat = rcen.dataPtr();
                const Real  mu      = visc_coef[comp];
                hoopsrc(ARLIM(lo), ARLIM(hi),
			vdat, ARLIM(vlo), ARLIM(vhi),
			sdat, ARLIM(slo), ARLIM(shi),
			rcendat, &mu);
            }
        }
#endif

        MultiFab::Copy(visc_terms,visc_tmp,0,comp-src_comp,1,0);
    }
    else {
      int ngrow = visc_terms.nGrow();
      visc_terms.setVal(0.0,comp-src_comp,1,ngrow);
    }
#else
    // mlmg way ...
    if (is_diffusive[comp])
    {
      // fixme for EB? again guessing 
        int ng = 1;
	// should try visc_tmp.nGrow = 0
        MultiFab visc_tmp(grids,dmap,1,ng,MFInfo(),navier_stokes->Factory()),
	  s_tmp(grids,dmap,1,ng,MFInfo(),navier_stokes->Factory());
        //
        // Set up operator and apply to compute viscous terms.
        //
        const Real a = 0.0;
        const Real b = allnull ? -visc_coef[comp] : -1.0;

	LPInfo info;
	info.setAgglomeration(agglomeration);
	info.setConsolidation(consolidation);
	info.setMaxCoarseningLevel(0);
	// let MLMG take care of r-z 
	info.setMetricTerm(false);

#ifdef AMREX_USE_EB
	// create the right data holder for passing to MLEBABecLap
	amrex::Vector<const amrex::EBFArrayBoxFactory*> ebf(1);
	ebf[0] = &(dynamic_cast<EBFArrayBoxFactory const&>(navier_stokes->Factory()));
	
	MLEBABecLap mlabec({navier_stokes->Geom()}, {grids}, {dmap}, info, ebf);
#else	  
	MLABecLaplacian mlabec({navier_stokes->Geom()},{grids},{dmap},info);
#endif
	// default max_order=2
	// mfix says:
	// It is essential that we set MaxOrder of the solver to 2
	// if we want to use the standard sol(i)-sol(i-1) approximation
	// for the gradient at Dirichlet boundaries.
	// The solver's default order is 3 and this uses three points for the
	// gradient at a Dirichlet boundary.
	mlabec.setMaxOrder(max_order);
	
	{
	  // set BCs

	  std::array<LinOpBCType,AMREX_SPACEDIM> mlmg_lobc;
	  std::array<LinOpBCType,AMREX_SPACEDIM> mlmg_hibc;
	  setDomainBC(mlmg_lobc, mlmg_hibc, comp);
	  
	  mlabec.setDomainBC(mlmg_lobc, mlmg_hibc);
  
	  MultiFab crsedata;
	    
	  if (level > 0) {
	    auto& crse_ns = *(coarser->navier_stokes);
	    crsedata.define(crse_ns.boxArray(), crse_ns.DistributionMap(), 1, ng,MFInfo(),crse_ns.Factory());
	    AmrLevel::FillPatch(crse_ns,crsedata,ng,time,State_Type,comp,1);
	    if (rho_flag == 2) {
	      // We want to evaluate (div beta grad) S, not rho*S.
	      const MultiFab& rhotime = crse_ns.get_rho(time);
	      MultiFab::Divide(crsedata,rhotime,0,0,1,ng);
	    }
	    mlabec.setCoarseFineBC(&crsedata, crse_ratio[0]);
	  }
	  
	  AmrLevel::FillPatch(*navier_stokes,s_tmp,ng,time,State_Type,comp,1);
	  if (rho_flag == 2) {
	    const MultiFab& rhotime = navier_stokes->get_rho(time);
	    MultiFab::Divide(s_tmp,rhotime,0,0,1,ng);
	  }
	  // fixme? Do we need/want this???
	  // mfix does this
	  //EB_set_covered(s_tmp, 0, 1, ng, covered_val);
	  //s_tmp.FillBoundary (geom.periodicity());
	  mlabec.setLevelBC(0, &s_tmp);
	}
	
	mlabec.setScalars(a,b);
	// mlabec.setACoeffs() not needed since a = 0.0 

	{
	  const MultiFab* area   = navier_stokes->Area();
	  const MultiFab *ap[AMREX_SPACEDIM];
	  for (int d=0; d<AMREX_SPACEDIM; ++d)
	  {
	    ap[d] = &(area[d]);
	  }
	  std::array<MultiFab,BL_SPACEDIM> bcoeffs;
	  for (int n = 0; n < BL_SPACEDIM; n++)
	  {
	    bcoeffs[n].define(area[n].boxArray(),area[n].DistributionMap(),1,0,MFInfo(),navier_stokes->Factory());
	  }
	  computeBeta(bcoeffs,beta,betaComp,navier_stokes->Geom(),ap,
		      parent->Geom(0).IsRZ());
	  //computeBeta(bcoeffs, beta, betaComp);
	  mlabec.setBCoeffs(0, amrex::GetArrOfConstPtrs(bcoeffs));
	}

	// Do we need something like this cribbed from mfix???
	// This sets the coefficient on the wall and defines it as a homogeneous
	// Dirichlet bc for the solve. mu_g is the viscosity at cc in mfix
	// matches what's in bcoeff
	//mlabec.setEBHomogDirichlet ( 0, (*mu_g[lev]) );

	MLMG mgn(mlabec);
	mgn.setVerbose(verbose);

	mgn.apply({&visc_tmp},{&s_tmp});

	//fixme
	// // compare to old method...
	// VisMF::Write(visc_terms,"VT");
	// VisMF::Write(visc_tmp,"VTtmp");
        // MultiFab diff(grids,dmap,1,1);
	// MultiFab::Copy(diff,visc_tmp,0,0,1,0);
	// MultiFab::Subtract(diff,visc_terms,comp-src_comp,0,1,0);
	// VisMF::Write(diff,"VTdiff");
	// std::cout << "Min and max of the diff are " << diff.min(0,0) <<" "
	// 	  <<diff.max(0,0)<<"\n";
	// MultiFab::Copy(diff,visc_tmp,0,0,1,0);
	// MultiFab::Divide(diff,visc_terms,comp-src_comp,0,1,0);
	// VisMF::Write(diff,"VTdiff2");
	///
	MultiFab::Copy(visc_terms,visc_tmp,0,comp-src_comp,1,0);
    }
    else {
      int ngrow = visc_terms.nGrow();
      visc_terms.setVal(0.0,comp-src_comp,1,ngrow);
    }
#endif
}

void
Diffusion::getTensorViscTerms (MultiFab&              visc_terms, 
                               Real                   time,
                               const MultiFab* const* beta,
                               int                    betaComp)
{
    const MultiFab* area   = navier_stokes->Area();
    // need for computeBeta. Don't see Why computeBeta defines area in this way
    // or why it even bothers to pass area when it's also passing geom
    const MultiFab *ap[AMREX_SPACEDIM];
    for (int d=0; d<AMREX_SPACEDIM; ++d)
    {
	ap[d] = &(area[d]);
    }

    int allthere;
    checkBeta(beta, allthere);

    const int src_comp = Xvel;
    const int ncomp    = visc_terms.nComp();
    
    if (ncomp < BL_SPACEDIM)
        amrex::Abort("Diffusion::getTensorViscTerms(): visc_terms needs at least BL_SPACEDIM components");
    //
    // Before computing the godunov predicitors we may have to
    // precompute the viscous source terms.  To do this we must
    // construct a Laplacian operator, set the coeficients and apply
    // it to the time N data.  First, however, we must precompute the
    // fine N bndry values.  We will do this for each scalar that diffuses.
    //
    // Note: This routine DOES NOT fill grow cells
    //
    MultiFab&   S    = navier_stokes->get_data(State_Type,time);
    //
    // FIXME
    // LinOp classes cannot handle multcomponent MultiFabs yet,
    // construct the components one at a time and copy to visc_terms.
    //
    if (is_diffusive[src_comp])
    {
        int ng = 1;
	MultiFab visc_tmp(grids,dmap,BL_SPACEDIM,0,MFInfo(),navier_stokes->Factory()),
	  //old way
	s_tmp(grids,dmap,BL_SPACEDIM,ng,MFInfo(),navier_stokes->Factory());
	MultiFab::Copy(s_tmp,S,Xvel,0,BL_SPACEDIM,0);
	// need to create an alias multifab
	//MultiFab s_tmp(S, amrex::make_alias, Xvel, AMREX_SPACEDIM);

	//fixme? not sure if we need this
	visc_tmp.setVal(0.);  

        //
        // Set up operator and apply to compute viscous terms.
        //
        const Real a = 0.0;
        const Real b = -1.0;

	// MLMG tensor solver
      {	
	LPInfo info;
	info.setAgglomeration(agglomeration);
	info.setConsolidation(consolidation);
	info.setMaxCoarseningLevel(0);
	info.setMetricTerm(false);
	
#ifdef AMREX_USE_EB
	const auto& ebf = &dynamic_cast<EBFArrayBoxFactory const&>(navier_stokes->Factory());
	MLEBTensorOp tensorop({navier_stokes->Geom()}, {grids}, {dmap}, info, {ebf});
#else
	MLTensorOp tensorop({navier_stokes->Geom()}, {grids}, {dmap}, info);
#endif	
	tensorop.setMaxOrder(tensor_max_order);
	
	// create right container
	Array<LinOpBCType,AMREX_SPACEDIM> mlmg_lobc[AMREX_SPACEDIM];
	Array<LinOpBCType,AMREX_SPACEDIM> mlmg_hibc[AMREX_SPACEDIM];
	// fill it
	for (int i=0; i<AMREX_SPACEDIM; i++)
	  setDomainBC(mlmg_lobc[i], mlmg_hibc[i], Xvel+i);
	// pass to op
	tensorop.setDomainBC({AMREX_D_DECL(mlmg_lobc[0],mlmg_lobc[1],mlmg_lobc[2])},
			     {AMREX_D_DECL(mlmg_hibc[0],mlmg_hibc[1],mlmg_hibc[2])});
	
	// set coarse-fine BCs
	{	    
	  MultiFab crsedata;
	  
	  if (level > 0) {
	    auto& crse_ns = *(coarser->navier_stokes);
	    crsedata.define(crse_ns.boxArray(), crse_ns.DistributionMap(),
			    AMREX_SPACEDIM, ng, MFInfo(),crse_ns.Factory());
	    AmrLevel::FillPatch(crse_ns, crsedata, ng, time, State_Type, Xvel,
				AMREX_SPACEDIM);
	    
	    tensorop.setCoarseFineBC(&crsedata, crse_ratio[0]);
	  }
	    
	  AmrLevel::FillPatch(*navier_stokes,s_tmp,ng,time,State_Type,Xvel,AMREX_SPACEDIM);
	  
	  // fixme? Do we need/want this
	  // seems like this ought be to have been done in FillPatch...
	  // EB_set_covered(Soln, 0, AMREX_SPACEDIM, ng, 1.2345e30);
	  ///

	  tensorop.setLevelBC(0, &s_tmp);
	  
	  // FIXME: check divergence of vel
	  // MLNodeLaplacian mllap({navier_stokes->Geom()}, {grids}, {dmap}, info);
	  // mllap.setDomainBC(mlmg_lobc[0], mlmg_hibc[0]);
	  // Rhs2.setVal(0.);
	  // mllap.compDivergence({&Rhs2}, {&s_tmp});
	  // amrex::WriteSingleLevelPlotfile("div_"+std::to_string(count), Rhs2, {AMREX_D_DECL("x","y","z")},navier_stokes->Geom(), 0.0, 0);
	  //
	}

	tensorop.setScalars(a, b);
	
	Array<MultiFab,AMREX_SPACEDIM> face_bcoef;
	for (int n = 0; n < BL_SPACEDIM; n++)
	{
	  face_bcoef[n].define(area[n].boxArray(),area[n].DistributionMap(),1,0,MFInfo(),navier_stokes->Factory());
	}
	computeBeta(face_bcoef,beta,betaComp,navier_stokes->Geom(),ap,
		    parent->Geom(0).IsRZ());
	//computeBeta(face_bcoef,beta,betaComp);
	
	tensorop.setShearViscosity(0, amrex::GetArrOfConstPtrs(face_bcoef));
	// FIXME??? Hack 
	// remove the "divmusi" terms by setting kappa = (2/3) mu
	//  
	Print()<<"WARNING: Hack to get rid of divU terms ...\n";
	Array<MultiFab,AMREX_SPACEDIM> kappa;
	Real twothirds = 2.0/3.0;
	for (int idim = 0; idim < AMREX_SPACEDIM; ++idim)
	{
	  kappa[idim].define(face_bcoef[idim].boxArray(), face_bcoef[idim].DistributionMap(), 1, 0, MFInfo(),navier_stokes->Factory());
	  MultiFab::Copy(kappa[idim], face_bcoef[idim], 0, 0, 1, 0);
	  kappa[idim].mult(twothirds);
	}
	// bulk viscosity not ususally needed for gasses
	tensorop.setBulkViscosity(0, amrex::GetArrOfConstPtrs(kappa));

#ifdef AMREX_USE_EB
	MultiFab cc_bcoef(grids,dmap,BL_SPACEDIM,0,MFInfo(),navier_stokes->Factory());
	EB_average_face_to_cellcenter(cc_bcoef, 0,
				      amrex::GetArrOfConstPtrs(face_bcoef));
	tensorop.setEBShearViscosity(0, cc_bcoef);
	// part of hack to remove divmusi terms
	cc_bcoef.mult(twothirds);
	tensorop.setEBBulkViscosity(0, cc_bcoef);
#endif
	  
	MLMG mlmg(tensorop);
	// FIXME -- consider making new parameters max_iter and bottom_verbose
	//mlmg.setMaxIter(max_iter);
	mlmg.setMaxFmgIter(max_fmg_iter);
	mlmg.setVerbose(10);
	mlmg.setBottomVerbose(10);
	//mlmg.setBottomVerbose(bottom_verbose);
	  
	mlmg.apply({&visc_tmp}, {&s_tmp});
      }

#if (BL_SPACEDIM == 2)
        if (parent->Geom(0).IsRZ())
        {
            int fort_xvel_comp = Xvel+1;

#ifdef _OPENMP
#pragma omp parallel
#endif
            for (MFIter vmfi(visc_tmp,true); vmfi.isValid(); ++vmfi)
            {
                const int  k   = vmfi.index();
                const Box& bx  = vmfi.tilebox();
		const Box& tmpbx  = vmfi.validbox();
                Box        vbx = amrex::grow(tmpbx,visc_tmp.nGrow());
                Box        sbx = amrex::grow(s_tmp.box(k),s_tmp.nGrow());

		Vector<Real> rcen;
                rcen.resize(bx.length(0));

                navier_stokes->Geom().GetCellLoc(rcen, bx, 0);

                const int*       lo        = bx.loVect();
                const int*       hi        = bx.hiVect();
                const int*       vlo       = vbx.loVect();
                const int*       vhi       = vbx.hiVect();
                const int*       slo       = sbx.loVect();
                const int*       shi       = sbx.hiVect();
                Real*            vdat      = visc_tmp[vmfi].dataPtr();
                Real*            sdat      = s_tmp[vmfi].dataPtr();
                const Real*      rcendat   = rcen.dataPtr();
                const FArrayBox& betax     = (*beta[0])[vmfi];
                const Real*      betax_dat = betax.dataPtr(betaComp);
                const int*       betax_lo  = betax.loVect();
                const int*       betax_hi  = betax.hiVect();
                const FArrayBox& betay     = (*beta[1])[vmfi];
                const Real*      betay_dat = betay.dataPtr(betaComp);
                const int*       betay_lo  = betay.loVect();
                const int*       betay_hi  = betay.hiVect();

                tensor_hoopsrc(&fort_xvel_comp,ARLIM(lo), ARLIM(hi),
			       vdat, ARLIM(vlo), ARLIM(vhi),
			       sdat, ARLIM(slo), ARLIM(shi),
			       rcendat, 
			       betax_dat,ARLIM(betax_lo),ARLIM(betax_hi),
			       betay_dat,ARLIM(betay_lo),ARLIM(betay_hi));
            }
        }
#endif
        MultiFab::Copy(visc_terms,visc_tmp,0,0,BL_SPACEDIM,0);
    }
    else
    {
        int ngrow = visc_terms.nGrow();
        visc_terms.setVal(0.0,src_comp,BL_SPACEDIM,ngrow);
    }
}

#include <AMReX_Utility.H>

void
Diffusion::getBndryData (ViscBndry& bndry,
                         int        src_comp,
                         int        num_comp,
                         Real       time,
                         int        rho_flag)
{
    BL_ASSERT(num_comp == 1);
    //
    // Fill phys bndry vals of grow cells of (tmp) multifab passed to bndry.
    //
    // TODO -- A MultiFab is a huge amount of space in which to pass along
    // the phys bc's.  InterpBndryData needs a more efficient interface.
    //
    const int     nGrow = 1;
    const BCRec&  bc    = navier_stokes->get_desc_lst()[State_Type].getBC(src_comp);

    bndry.define(grids,dmap,num_comp,navier_stokes->Geom());

    const MultiFab& rhotime = navier_stokes->get_rho(time);

    MultiFab S(grids, dmap, num_comp, nGrow,MFInfo(),navier_stokes->Factory());

    AmrLevel::FillPatch(*navier_stokes,S,nGrow,time,State_Type,src_comp,num_comp);

    if (rho_flag == 2) {
        for (int n = 0; n < num_comp; ++n) {
	    MultiFab::Divide(S,rhotime,0,n,1,nGrow);
	}
    }

    S.setVal(BL_SAFE_BOGUS, 0, num_comp, 0);
    
    if (level == 0)
    {
        bndry.setBndryValues(S,0,0,num_comp,bc);
    }
    else
    {
        BoxArray cgrids = grids;
        cgrids.coarsen(crse_ratio);
        BndryRegister crse_br(cgrids,dmap,0,1,2,num_comp);
        //
        // interp for solvers over ALL c-f brs, need safe data.
        //
        crse_br.setVal(BL_BOGUS);
        coarser->FillBoundary(crse_br,src_comp,0,num_comp,time,rho_flag);
        bndry.setBndryValues(crse_br,0,S,0,0,num_comp,crse_ratio,bc);
    }
}

void
Diffusion::getBndryDataGivenS (ViscBndry& bndry,
                               MultiFab&  Rho_and_spec,
                               MultiFab&  Rho_and_spec_crse,
                               int        state_ind,
                               int        src_comp,
                               int        num_comp)
{
    BL_ASSERT(num_comp == 1);
    const int     nGrow = 1;
    //
    // Fill phys bndry vals of grow cells of (tmp) multifab passed to bndry.
    //
    // TODO -- A MultiFab is a huge amount of space in which to pass along
    // the phys bc's.  InterpBndryData needs a more efficient interface.
    //
    const BCRec& bc = navier_stokes->get_desc_lst()[State_Type].getBC(state_ind);

    bndry.define(grids,dmap,num_comp,navier_stokes->Geom());

    if (level == 0)
    {
        bndry.setBndryValues(Rho_and_spec,src_comp,0,num_comp,bc);
    }
    else
    {
        BoxArray cgrids = grids;
        cgrids.coarsen(crse_ratio);
        BndryRegister crse_br(cgrids,dmap,0,1,2,num_comp);
        //
        // interp for solvers over ALL c-f brs, need safe data.
        //
        crse_br.setVal(BL_BOGUS);
        crse_br.copyFrom(Rho_and_spec_crse,nGrow,src_comp,0,num_comp);
        bndry.setBndryValues(crse_br,0,Rho_and_spec,src_comp,0,num_comp,crse_ratio,bc);
    }
}

void
Diffusion::FillBoundary (BndryRegister& bdry,
                         int            state_ind,
                         int            dest_comp,
                         int            num_comp,
                         Real           time,
                         int            rho_flag)
{
    //
    // Need one grow cell filled for linear solvers.
    // We assume filPatch gets this right, where possible.
    //
    const int     nGrow = 1;

    const MultiFab& rhotime = navier_stokes->get_rho(time);

    MultiFab S(navier_stokes->boxArray(),
               navier_stokes->DistributionMap(),
               num_comp,nGrow,MFInfo(),navier_stokes->Factory());

    AmrLevel::FillPatch(*navier_stokes,S,nGrow,time,State_Type,state_ind,num_comp);

    if (rho_flag == 2) {
        for (int n = 0; n < num_comp; ++n) {
	    MultiFab::Divide(S,rhotime,0,n,1,nGrow);
	}
    }

    //
    // Copy into boundary register.
    //
    bdry.copyFrom(S,nGrow,0,dest_comp,num_comp);
    
}

void
Diffusion::getTensorBndryData (ViscBndryTensor& bndry, 
                               Real             time)
{
    const int num_comp = BL_SPACEDIM;
    const int src_comp = Xvel;
    const int nDer     = MCLinOp::bcComponentsNeeded();
    //
    // Create the BCRec's interpreted by ViscBndry objects
    //
    Vector<BCRec> bcarray(nDer, BCRec(D_DECL(EXT_DIR,EXT_DIR,EXT_DIR),
                                     D_DECL(EXT_DIR,EXT_DIR,EXT_DIR)));

    for (int idim = 0; idim < BL_SPACEDIM; idim++)
        bcarray[idim] = navier_stokes->get_desc_lst()[State_Type].getBC(src_comp+idim);

    bndry.define(grids,dmap,nDer,navier_stokes->Geom());

    const int nGrow = 1;

    MultiFab S(grids,dmap,num_comp,nGrow,MFInfo(),navier_stokes->Factory());

    AmrLevel::FillPatch(*navier_stokes,S,nGrow,time,State_Type,src_comp,num_comp);

    S.setVal(BL_SAFE_BOGUS, 0, num_comp, 0);
    
    if (level == 0)
    {
        bndry.setBndryValues(S,0,0,num_comp,bcarray);
    }
    else
    {
        BoxArray cgrids(grids);
        cgrids.coarsen(crse_ratio);
        BndryRegister crse_br(cgrids,dmap,0,1,1,num_comp);
        crse_br.setVal(BL_BOGUS);
        const int rho_flag = 0;
        coarser->FillBoundary(crse_br,src_comp,0,num_comp,time,rho_flag);
        bndry.setBndryValues(crse_br,0,S,0,0,num_comp,crse_ratio[0],bcarray);
    }
}

void
Diffusion::checkBetas (const MultiFab* const* beta1, 
                       const MultiFab* const* beta2,
                       int&                   allthere,
                       int&                   allnull) const
{
    int allnull1, allnull2, allthere1, allthere2;

    checkBeta(beta1,allthere1,allnull1);
    checkBeta(beta2,allthere2,allnull2);
    allnull  = allnull1 && allnull2;
    allthere = allthere1 && allthere2;

    if (!(allthere || allnull))
        amrex::Abort("Diffusion::checkBetas(): betas must either be all 0 or all non-0");
}

void
Diffusion::checkBeta (const MultiFab* const* beta,
                      int&                   allthere,
                      int&                   allnull) //const
{
    allnull  = 1;
    allthere = beta != 0;

    if (allthere)
    {
        for (int d = 0; d < BL_SPACEDIM; d++)
        {
            allnull = allnull && beta[d] == 0;
            allthere = allthere && beta[d] != 0;
        }
    }

    if (!(allthere || allnull))
        amrex::Abort("Diffusion::checkBeta(): betas must be all 0 or all non-0");
}

void
Diffusion::checkBeta (const MultiFab* const* beta,
                      int&                   allthere) const
{
    allthere = beta != 0;

    if (allthere)
    {
        for (int d = 0; d < BL_SPACEDIM; d++)
            allthere = allthere && beta[d] != 0;
    }

    if (!allthere)
        amrex::Abort("Diffusion::checkBeta(): betas must be all non-0");
}

//
// This routine computes the vector div mu SI, where I is the identity 
// tensor, S = div U, and mu is constant.
//

void
Diffusion::compute_divmusi (Real      time,
                            Real      mu,
                            MultiFab& divmusi)
{
    //
    // Compute on valid region.  Calling function should fill grow cells.
    //
    if (mu > 0.0)
    {
        const int     nGrowDU  = 1;
        const Real*   dx       = navier_stokes->Geom().CellSize();
        std::unique_ptr<MultiFab> divu_fp ( navier_stokes->getDivCond(nGrowDU,time) );

#ifdef _OPENMP
#pragma omp parallel
#endif
        for (MFIter divmusimfi(divmusi,true); divmusimfi.isValid(); ++divmusimfi)
        {
            FArrayBox& fab  = divmusi[divmusimfi];
            FArrayBox& divu = (*divu_fp)[divmusimfi];
            const Box& box  = divmusimfi.tilebox();

            div_mu_si(box.loVect(), box.hiVect(), dx, &mu,
		      ARLIM(divu.loVect()), ARLIM(divu.hiVect()),
		      divu.dataPtr(),
		      ARLIM(fab.loVect()),  ARLIM(fab.hiVect()),
		      fab.dataPtr());
        }
    }
    else
    {
        const int nGrow = 0; // Not to fill grow cells here
        divmusi.setVal(0,nGrow);
    }
}

//
// This routine computes the vector div beta SI, where I is the identity 
// tensor, S = div U, and beta is non-constant.
//

void
Diffusion::compute_divmusi (Real                   time,
                            const MultiFab* const* beta,
                            MultiFab&              divmusi)
{
    const int     nGrowDU  = 1;
    const Real*   dx       = navier_stokes->Geom().CellSize();
    std::unique_ptr<MultiFab> divu_fp ( navier_stokes->getDivCond(nGrowDU,time) );

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter divmusimfi(divmusi,true); divmusimfi.isValid(); ++divmusimfi)
    {
        FArrayBox& divu = (*divu_fp)[divmusimfi];
        const Box& box  = divmusimfi.tilebox();

        DEF_CLIMITS((*beta[0])[divmusimfi],betax,betaxlo,betaxhi);
        DEF_CLIMITS((*beta[1])[divmusimfi],betay,betaylo,betayhi);

#if (BL_SPACEDIM==3)
        DEF_CLIMITS((*beta[2])[divmusimfi],betaz,betazlo,betazhi);
#endif

        div_varmu_si(box.loVect(),box.hiVect(), dx,
		     ARLIM(divu.loVect()), ARLIM(divu.hiVect()),
		     divu.dataPtr(),
		     ARLIM(betaxlo), ARLIM(betaxhi), betax,
		     ARLIM(betaylo), ARLIM(betayhi), betay,
#if (BL_SPACEDIM==3)
		     ARLIM(betazlo), ARLIM(betazhi), betaz,
#endif
		     ARLIM(divmusi[divmusimfi].loVect()), ARLIM(divmusi[divmusimfi].hiVect()),
		     divmusi[divmusimfi].dataPtr());
    }
}


//
// SAS: The following function is a temporary fix in the migration from
//      using is_conservative and rho_flag over to using advectionType
//      and diffusionType.
//
int
Diffusion::set_rho_flag(const DiffusionForm compDiffusionType)
{
    int rho_flag = 0;

    switch (compDiffusionType)
    {
        case Laplacian_S:
            rho_flag = 0;
            break;

        case RhoInverse_Laplacian_S:
            rho_flag = 1;
            break;

        case Laplacian_SoverRho:
            rho_flag = 2;
            break;

	    //NOTE: rho_flag = 3 is used in a different context for
	    //      do_mom_diff==1
	    
        default:
            amrex::Print() << "compDiffusionType = " << compDiffusionType << '\n';
            amrex::Abort("An unknown NavierStokesBase::DiffusionForm was used in set_rho_flag");
    }

    return rho_flag;
}

bool
Diffusion::are_any(const Vector<DiffusionForm>& diffusionType,
                   const DiffusionForm         testForm,
                   const int                   sComp,
                   const int                   nComp)
{
    for (int comp = sComp; comp < sComp + nComp; ++comp)
    {
        if (diffusionType[comp] == testForm)
            return true;
    }

    return false;
}

int
Diffusion::how_many(const Vector<DiffusionForm>& diffusionType,
                    const DiffusionForm         testForm,
                    const int                   sComp,
                    const int                   nComp)
{
    int counter = 0;

    for (int comp = sComp; comp < sComp + nComp; ++comp)
    {
        if (diffusionType[comp] == testForm)
            ++counter;
    }

    return counter;
}

void
Diffusion::setDomainBC (std::array<LinOpBCType,AMREX_SPACEDIM>& mlmg_lobc,
                        std::array<LinOpBCType,AMREX_SPACEDIM>& mlmg_hibc,
                        int src_comp)
{
    const BCRec& bc = navier_stokes->get_desc_lst()[State_Type].getBC(src_comp);
    for (int idim = 0; idim < AMREX_SPACEDIM; ++idim)
    {
        if (parent->Geom(0).isPeriodic(idim))
        {
            mlmg_lobc[idim] = mlmg_hibc[idim] = LinOpBCType::Periodic;
        }
        else
        {
            int pbc = bc.lo(idim);
            if (pbc == EXT_DIR)
            {
                mlmg_lobc[idim] = LinOpBCType::Dirichlet;
            }
            else if (pbc == FOEXTRAP      ||
                     pbc == HOEXTRAP      || 
                     pbc == REFLECT_EVEN)
            {
                mlmg_lobc[idim] = LinOpBCType::Neumann;
            }
            else if (pbc == REFLECT_ODD)
            {
                mlmg_lobc[idim] = LinOpBCType::reflect_odd;
            }
            else
            {
                mlmg_lobc[idim] = LinOpBCType::bogus;
            }

            pbc = bc.hi(idim);
            if (pbc == EXT_DIR)
            {
                mlmg_hibc[idim] = LinOpBCType::Dirichlet;
            }
            else if (pbc == FOEXTRAP      ||
                     pbc == HOEXTRAP      || 
                     pbc == REFLECT_EVEN)
            {
                mlmg_hibc[idim] = LinOpBCType::Neumann;
            }
            else if (pbc == REFLECT_ODD)
            {
                mlmg_hibc[idim] = LinOpBCType::reflect_odd;
            }
            else
            {
                mlmg_hibc[idim] = LinOpBCType::bogus;
            }
        }
    }
}

void
Diffusion::setDomainBC (std::array<LinOpBCType,AMREX_SPACEDIM>& mlmg_lobc,
                            std::array<LinOpBCType,AMREX_SPACEDIM>& mlmg_hibc,
                            const BCRec& bc)
{

    for (int idim = 0; idim < AMREX_SPACEDIM; ++idim)
    {
      // fixme??? not sure DefaultGeometry really returns what's desired: parent->Geom(0)
      if (DefaultGeometry().isPeriodic(idim))
        {
            mlmg_lobc[idim] = mlmg_hibc[idim] = LinOpBCType::Periodic;
        }
        else
        {
            int pbc = bc.lo(idim);
            if (pbc == EXT_DIR)
            {
                mlmg_lobc[idim] = LinOpBCType::Dirichlet;
            }
            else if (pbc == FOEXTRAP      ||
                     pbc == HOEXTRAP      || 
                     pbc == REFLECT_EVEN)
            {
                mlmg_lobc[idim] = LinOpBCType::Neumann;
            }
            else if (pbc == REFLECT_ODD)
            {
                mlmg_lobc[idim] = LinOpBCType::reflect_odd;
            }
            else
            {
                mlmg_lobc[idim] = LinOpBCType::bogus;
            }

            pbc = bc.hi(idim);
            if (pbc == EXT_DIR)
            {
                mlmg_hibc[idim] = LinOpBCType::Dirichlet;
            }
            else if (pbc == FOEXTRAP      ||
                     pbc == HOEXTRAP      || 
                     pbc == REFLECT_EVEN)
            {
                mlmg_hibc[idim] = LinOpBCType::Neumann;
            }
            else if (pbc == REFLECT_ODD)
            {
                mlmg_hibc[idim] = LinOpBCType::reflect_odd;
            }
            else
            {
                mlmg_hibc[idim] = LinOpBCType::bogus;
            }
        }
    }
}


