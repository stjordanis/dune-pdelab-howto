// -*- tab-width: 4; indent-tabs-mode: nil -*-
/** \file 
    \brief Solve Poisson problem on various grids (sequential)
*/
#ifdef HAVE_CONFIG_H
#include "config.h"     
#endif
#include<iostream>
#include<vector>
#include<map>
#include<string>
#include<dune/common/mpihelper.hh>
#include<dune/common/exceptions.hh>
#include<dune/common/fvector.hh>
#include<dune/common/float_cmp.hh>
#include<dune/common/static_assert.hh>
#include<dune/grid/yaspgrid.hh>
#include<dune/grid/io/file/vtk/subsamplingvtkwriter.hh>
#include<dune/istl/bvector.hh>
#include<dune/istl/operators.hh>
#include<dune/istl/solvers.hh>
#include<dune/istl/preconditioners.hh>
#include<dune/istl/io.hh>

#include<dune/pdelab/finiteelementmap/p0fem.hh>
#include<dune/pdelab/finiteelementmap/p1fem.hh>
#include<dune/pdelab/finiteelementmap/pk2dfem.hh>
#include<dune/pdelab/finiteelementmap/pk3dfem.hh>
#include<dune/pdelab/finiteelementmap/q22dfem.hh>
#include<dune/pdelab/finiteelementmap/q1fem.hh>
#include<dune/pdelab/finiteelementmap/conformingconstraints.hh>
#include<dune/pdelab/finiteelementmap/hangingnodeconstraints.hh>
#include<dune/pdelab/gridfunctionspace/gridfunctionspace.hh>
#include<dune/pdelab/gridfunctionspace/gridfunctionspaceutilities.hh>
#include<dune/pdelab/gridfunctionspace/interpolate.hh>
#include<dune/pdelab/constraints/constraints.hh>
#include<dune/pdelab/common/function.hh>
#include<dune/pdelab/common/vtkexport.hh>
#include<dune/pdelab/backend/istlvectorbackend.hh>
#include<dune/pdelab/backend/istlmatrixbackend.hh>
#include<dune/pdelab/backend/istlsolverbackend.hh>
#include<dune/pdelab/localoperator/laplacedirichletp12d.hh>
#include<dune/pdelab/localoperator/poisson.hh>
#include<dune/pdelab/constraints/constraintsparameters.hh>
#include<dune/pdelab/gridoperator/gridoperator.hh>

#include "../utility/gridexamples.hh"



#define HANGING_NODES_REFINEMENT // Macro to switch on hanging nodes tests


//===============================================================
//===============================================================
// Solve the Poisson equation
//           - \Delta u = f in \Omega, 
//                    u = g on \partial\Omega_D
//  -\nabla u \cdot \nu = j on \partial\Omega_N
//===============================================================
//===============================================================

//===============================================================
// Define parameter functions f,g,j and \partial\Omega_D/N
//===============================================================

// function for defining the source term
template<typename GV, typename RF>
class F
  : public Dune::PDELab::AnalyticGridFunctionBase<Dune::PDELab::AnalyticGridFunctionTraits<GV,RF,1>,
                                                  F<GV,RF> >
{
public:
  typedef Dune::PDELab::AnalyticGridFunctionTraits<GV,RF,1> Traits;
  typedef Dune::PDELab::AnalyticGridFunctionBase<Traits,F<GV,RF> > BaseT;

  F (const GV& gv) : BaseT(gv) {}
  inline void evaluateGlobal (const typename Traits::DomainType& x, 
							  typename Traits::RangeType& y) const
  {
    if (x[0]>0.25 && x[0]<0.375 && x[1]>0.25 && x[1]<0.375)
      y = 50.0;
    else
      y = 0.0;
    y=0;
  }
};



// constraints parameter class for selecting boundary condition type 
class BCTypeParam
  : public Dune::PDELab::DirichletConstraintsParameters /*@\label{bcp:base}@*/
{
public:

  template<typename I>
  bool isDirichlet(
				   const I & intersection,   /*@\label{bcp:name}@*/
				   const Dune::FieldVector<typename I::ctype, I::dimension-1> & coord
				   ) const
  {
	
    Dune::FieldVector<typename I::ctype, I::dimension>
      xg = intersection.geometry().global( coord );
	
    if( xg[1]<1E-6 || xg[1]>1.0-1E-6 )
      return false; // Neumann b.c.
    else if( xg[0]>1.0-1E-6 && xg[1]>0.5+1E-6 )
      return false; // Neumann b.c.
    else
      return true;  // Dirichlet b.c. on all other boundaries
  }
};

// function for Dirichlet boundary conditions and initialization
template<typename GV, typename RF>
class G
  : public Dune::PDELab::AnalyticGridFunctionBase<Dune::PDELab::AnalyticGridFunctionTraits<GV,RF,1>,
                                                  G<GV,RF> >
{
public:
  typedef Dune::PDELab::AnalyticGridFunctionTraits<GV,RF,1> Traits;
  typedef Dune::PDELab::AnalyticGridFunctionBase<Traits,G<GV,RF> > BaseT;

  G (const GV& gv) : BaseT(gv) {}
  inline void evaluateGlobal (const typename Traits::DomainType& x, 
							  typename Traits::RangeType& y) const
  {
    typename Traits::DomainType center;
    for (int i=0; i<GV::dimension; i++) center[i] = 0.5;
    center -= x;
	y = exp(-center.two_norm2());
  }
};

// function for defining the flux boundary condition
template<typename GV, typename RF>
class J
  : public Dune::PDELab::AnalyticGridFunctionBase<Dune::PDELab::AnalyticGridFunctionTraits<GV,RF,1>,
                                                  J<GV,RF> >
{
public:
  typedef Dune::PDELab::AnalyticGridFunctionTraits<GV,RF,1> Traits;
  typedef Dune::PDELab::AnalyticGridFunctionBase<Traits,J<GV,RF> > BaseT;

  J (const GV& gv) : BaseT(gv) {}
  inline void evaluateGlobal (const typename Traits::DomainType& x, 
							  typename Traits::RangeType& y) const
  {
    if (x[1]<1E-6 || x[1]>1.0-1E-6)
      {
        y = 0;
        return;
      }
    if (x[0]>1.0-1E-6 && x[1]>0.5+1E-6)
      {
        y = -5.0;
        return;
      }
  }
};







//===============================================================
// Problem setup and solution 
//===============================================================

// generate a P1 function and output it
template<typename GV, typename FEM, typename BCTYPE, typename CON, int q> 
void poisson( const GV& gv, 
              const FEM& fem, 
              std::string filename, 
              const BCTYPE& bctype,    // boundary condition type
              const CON& con = CON() )
{
  // constants and types
  typedef typename GV::Grid::ctype DF;
  typedef typename FEM::Traits::FiniteElementType::Traits::
    LocalBasisType::Traits::RangeFieldType R;

  // make grid function space
  typedef Dune::PDELab::ISTLVectorBackend<1> VBE;
  typedef Dune::PDELab::GridFunctionSpace<GV,FEM,CON,VBE> GFS; 
  GFS gfs(gv,fem,con);

  // make constraints map and initialize it from a function
  typedef typename GFS::template ConstraintsContainer<R>::Type C;
  C cg;
  cg.clear();

  Dune::PDELab::constraints(bctype,gfs,cg);

  // make grid operator
  typedef F<GV,R> FType;
  FType f(gv);
  typedef J<GV,R> JType;
  JType j(gv);
  typedef Dune::PDELab::Poisson<FType,BCTypeParam,JType,q> LOP; 
  LOP lop(f,bctype,j);

  typedef Dune::PDELab::GridOperator<GFS,GFS,LOP,VBE::MatrixBackend,R,R,R,C,C> GO;
  GO go(gfs,cg,gfs,cg,lop);

  // make coefficent Vector and initialize it from a function
  typedef typename GO::Traits::Domain V;
  V x0(gfs);
  x0 = 0.0;
  typedef G<GV,R> GType;
  GType g(gv);
  Dune::PDELab::interpolate(g,gfs,x0);
  Dune::PDELab::set_shifted_dofs(cg,0.0,x0);

  // represent operator as a matrix
  typedef typename GO::Jacobian M;
  M m(go);
  m = 0.0;

  // For hangingnodes: Interpolate hangingnodes adajcent to dirichlet
  // nodes
  go.localAssembler().backtransform(x0);
  
  go.jacobian(x0,m);
  //  Dune::printmatrix(std::cout,m.base(),"global stiffness matrix","row",9,1);

  // evaluate residual w.r.t initial guess
  V r(gfs);
  r = 0.0;

  go.residual(x0,r);

  // make ISTL solver
  Dune::MatrixAdapter<M,V,V> opa(m);
//   typedef Dune::PDELab::OnTheFlyOperator<V,V,GOS> ISTLOnTheFlyOperator;
//   ISTLOnTheFlyOperator opb(gos);
  Dune::SeqSSOR<M,V,V> ssor(m,1,1.0);
//   Dune::SeqILU0<M,V,V> ilu0(m,1.0);
//   Dune::Richardson<V,V> richardson(1.0);

  Dune::CGSolver<V> solvera(opa,ssor,1E-10,5000,2);
  //  Dune::CGSolver<V> solverb(opb,richardson,1E-10,5000,2);
  Dune::BiCGSTABSolver<V> solverc(opa,ssor,1E-10,5000,2);
  Dune::InverseOperatorResult stat;

  // solve the jacobian system
  r *= -1.0; // need -residual
  V x(gfs,0.0);
  solvera.apply(x,r,stat);

  // For hangingnodes: Set values of hangingnodes to zero
  Dune::PDELab::set_shifted_dofs(cg,0.0,x0);

  x += x0; //affine shift

  // Transform solution into standard basis
  go.localAssembler().backtransform(x);

  // make discrete function object
  typedef Dune::PDELab::DiscreteGridFunction<GFS,V> DGF;
  DGF dgf(gfs,x);

  // output grid function with VTKWriter
  Dune::VTKWriter<GV> vtkwriter(gv,Dune::VTKOptions::conforming);
  //Dune::SubsamplingVTKWriter<GV> vtkwriter( gv, 1 );
  vtkwriter.addVertexData(new Dune::PDELab::VTKGridFunctionAdapter<DGF>(dgf,"solution"));
  vtkwriter.write(filename,Dune::VTKOptions::ascii);
}







#ifdef HANGING_NODES_REFINEMENT

template<typename Grid, typename Iterator>
void doSomeRandomRefinement( Grid & grid ){

  // Do some random refinement. The result is a grid that may
  // contain multiple hanging nodes per edge!

  for(int i=0; i<4;++i){
    Iterator it = grid.template leafbegin<0,Dune::All_Partition>();
    Iterator eit = grid.template leafend<0,Dune::All_Partition>();
    
    for(;it!=eit;++it){
      if((double)rand()/(double)RAND_MAX > 0.6)
        grid.mark(1,*(it));
    }
    grid.preAdapt();
    grid.adapt();
    grid.postAdapt();
  }

}

#endif









//===============================================================
// Main program with grid setup
//===============================================================
// The domain is always the unit square in 2D or the unit cube in 3D
//
// Overview:
//
// Testcase 1.) ALUGrid 2D triangular cells (hanging nodes refinement) - Pk elements
// Testcase 2.) ALUGrid 3D cubical cells (hanging nodes refinement) - Q1 elements
// Testcase 3.) ALUGrid 3D tetrahedral cells (uniform refinement) - Pk elements
//
// Testcase 4.) YaspGrid 2D rectangular cells (uniform refinement) - Q1 elements
// Testcase 5.) YaspGrid 2D rectangular cells (uniform refinement) - Q2 elements
// Testcase 6.) YaspGrid 3D rectangular cells (uniform refinement) - Q1 elements
//
// Testcase  7.) UG 2D triangular cells (hanging nodes refinement) - P1 elements
// Testcase  8.) UG 2D rectangular cells (hanging nodes refinement) - Q1 elements
// Testcase  9.) UG 3D cubical cells (hanging nodes refinement) - Q1 elements
// Testcase 10.) UG 3D tetrahedral cells (hanging nodes refinement) - P1 elements
//
// Testcase 11.) Alberta Grid 2D triangular cells (uniform refinement) - Pk elements
// 
// Not supported by the Grid: ALUGrid 2D rectangular cells

int main(int argc, char** argv)
{
  try{
    //Maybe initialize Mpi
    Dune::MPIHelper::instance(argc, argv);

#if HAVE_ALUGRID
    {
#ifdef HANGING_NODES_REFINEMENT
      std::cout 
        << std::endl << std::endl
        << "Testcase 1.) ALUGrid 2D triangular cells (hanging nodes refinement) - P1 elements" 
        << std::endl;
#else
      std::cout 
        << std::endl << std::endl
        << "Testcase 1.) ALUGrid 2D triangular cells (uniform refinement) - P1 elements" 
        << std::endl;
#endif

      // make grid 
      typedef ALUUnitSquare Grid;
      Grid grid;
      grid.globalRefine(4);

      typedef ALUUnitSquare::Codim<0>::Partition<Dune::All_Partition>::LeafIterator Iterator;
      typedef ALUUnitSquare::LeafIntersectionIterator IntersectionIterator;
      typedef ALUUnitSquare::LeafGridView GV;
      typedef ALUUnitSquare::ctype ctype;

      // get view
      const GV& gv=grid.leafView();

#ifdef HANGING_NODES_REFINEMENT
      doSomeRandomRefinement<Grid,Iterator>( grid );
#endif
      // make finite element map
      typedef GV::Grid::ctype DF;
      typedef double R;
      const int k=1;
      const int q=2*k;
      typedef Dune::PDELab::Pk2DLocalFiniteElementMap<GV,DF,double,k> FEM;
      FEM fem(gv);

      // We need the boundary function for the hanging nodes
      // constraints engine as we have to distinguish between hanging
      // nodes on dirichlet and on neumann boundaries
      BCTypeParam bctype;

#ifdef HANGING_NODES_REFINEMENT
      // This is the type of the local constraints assembler that has
      // to be adapted for different local basis spaces and grid types
      typedef Dune::PDELab::HangingNodesConstraintsAssemblers::SimplexGridP1Assembler ConstraintsAssembler;

      // The type of the constraints engine
      typedef Dune::PDELab::HangingNodesDirichletConstraints
        <GV::Grid,ConstraintsAssembler,BCTypeParam> Constraints;

      // Get constraints engine. We set adaptToIsolateHangingNodes =
      // true and therefore the constructor refines the grid until
      // there are less than two hanging nodes per edge.
      Constraints constraints(grid,true,bctype);

      poisson<GV,FEM,BCTypeParam,Constraints,q>( gv,
                                                 fem,
                                                 "poisson_ALU_Pk_2d_hangingNodes",
                                                 bctype,
                                                 constraints
                                                 );
#else
      typedef Dune::PDELab::ConformingDirichletConstraints Constraints;
      poisson<GV,FEM,BCTypeParam,Constraints,q>(gv,fem,"poisson_ALU_Pk_2d",bctype);
#endif
    }
#endif


#if HAVE_ALUGRID
    {
#ifdef HANGING_NODES_REFINEMENT
      std::cout 
        << std::endl << std::endl
        << "Testcase 2.) ALUGrid 3D cubical cells (hanging nodes refinement) - Q1 elements" 
        << std::endl;
#else
      std::cout 
        << std::endl << std::endl
        << "Alternative Testcase 2.) ALUGrid 3D cubical cells (uniform refinement) - Q1 elements" 
        << std::endl;
#endif

      // make grid 
      typedef ALUCubeUnitSquare Grid;
      Grid grid;
      grid.globalRefine(1);
      
      typedef ALUCubeUnitSquare::Codim<0>::Partition<Dune::All_Partition>::LeafIterator 
        Iterator;
      typedef ALUCubeUnitSquare::LeafIntersectionIterator IntersectionIterator;
      typedef ALUCubeUnitSquare::LeafGridView GV;
      typedef ALUCubeUnitSquare::ctype ctype;

      // get view
      const GV& gv=grid.leafView(); 

#ifdef HANGING_NODES_REFINEMENT
      doSomeRandomRefinement<Grid,Iterator>( grid );
#endif

      // make finite element map
      typedef GV::Grid::ctype DF;
      typedef double R;
      const int q=2;
      typedef Dune::PDELab::Q1LocalFiniteElementMap<DF,R,3> FEM;
      FEM fem;

      BCTypeParam bctype;
      
#ifdef HANGING_NODES_REFINEMENT
      typedef Dune::PDELab::HangingNodesConstraintsAssemblers::CubeGridQ1Assembler ConstraintsAssembler;
      typedef Dune::PDELab::HangingNodesDirichletConstraints
        <GV::Grid,ConstraintsAssembler,BCTypeParam> Constraints;
      
      Constraints constraints(grid,true,bctype);
      
      // solve problem
      poisson<GV,FEM,BCTypeParam,Constraints,q>( gv,
                                                 fem,
                                                 "poisson_ALU_Q1_3d_hangingNodes",
                                                 bctype,
                                                 constraints
                                                 );
#else
      typedef Dune::PDELab::ConformingDirichletConstraints Constraints;
      poisson<GV,FEM,BCTypeParam,Constraints,q>(gv,fem,"poisson_ALU_Q1_3d",bctype);
#endif

    }
#endif


#if HAVE_ALUGRID
    {
      std::cout 
        << std::endl << std::endl
        << "Testcase 3.) ALUGrid 3D tetrahedral cells (uniform refinement) - Pk elements" 
        << std::endl;

      // make grid
      typedef ALUUnitCube<3> UnitCube;
      UnitCube unitcube;
      unitcube.grid().globalRefine(2);

      // get view
      typedef UnitCube::GridType::LeafGridView GV;
      const GV& gv=unitcube.grid().leafView(); 
 
      // make finite element map
      
      typedef UnitCube::GridType::ctype DF;
      typedef double R;
      const int k=4;
      const int q=2*k;
      typedef Dune::PDELab::Pk3DLocalFiniteElementMap<GV,DF,R,k> FEM;
      FEM fem(gv);

      BCTypeParam bctype;
  
      // solve problem
      typedef Dune::PDELab::ConformingDirichletConstraints Constraints;
      poisson<GV,FEM,BCTypeParam,Constraints,q>( gv,fem,"poisson_ALU_Pk_3d", bctype );

    }
#endif


    //return 0;

#if HAVE_YASP
    {
      std::cout 
        << std::endl << std::endl
        << "Testcase 4.) YaspGrid 2D rectangular cells (uniform refinement) - Q1 elements" 
        << std::endl;

      // make grid
      Dune::FieldVector<double,2> L(1.0);
      Dune::FieldVector<int,2> N(1);
      Dune::FieldVector<bool,2> B(false);
      Dune::YaspGrid<2> grid(L,N,B,0);
      grid.globalRefine(6);

      // get view
      typedef Dune::YaspGrid<2>::LeafGridView GV;
      const GV& gv=grid.leafView(); 

      // make finite element map
      typedef GV::Grid::ctype DF;
      typedef Dune::PDELab::Q1LocalFiniteElementMap<DF,double> FEM;
      FEM fem;
  
      BCTypeParam bctype;
      // solve problem
      typedef Dune::PDELab::ConformingDirichletConstraints Constraints;
      poisson<GV,FEM,BCTypeParam,Constraints,2>(gv,fem,"poisson_yasp_Q1_2d",bctype);
    }


    {
      std::cout 
        << std::endl << std::endl
        << "Testcase 5.) YaspGrid 2D rectangular cells (uniform refinement) - Q2 elements" 
        << std::endl;

      // make grid
      Dune::FieldVector<double,2> L(1.0);
      Dune::FieldVector<int,2> N(1);
      Dune::FieldVector<bool,2> B(false);
      Dune::YaspGrid<2> grid(L,N,B,0);
      grid.globalRefine(3);

      // get view
      typedef Dune::YaspGrid<2>::LeafGridView GV;
      const GV& gv=grid.leafView(); 

      // make finite element map
      typedef GV::Grid::ctype DF;
      typedef Dune::PDELab::Q22DLocalFiniteElementMap<DF,double> FEM;
      FEM fem;
      BCTypeParam bctype;
  
      // solve problem
      typedef Dune::PDELab::ConformingDirichletConstraints Constraints;
      poisson<GV,FEM,BCTypeParam,Constraints,2>(gv,fem,"poisson_yasp_Q2_2d",bctype);
    }

    {
      std::cout 
        << std::endl << std::endl
        << "Testcase 6.) YaspGrid 3D rectangular cells (uniform refinement) - Q1 elements" 
        << std::endl;

      // make grid
      Dune::FieldVector<double,3> L(1.0);
      Dune::FieldVector<int,3> N(1);
      Dune::FieldVector<bool,3> B(false);
      Dune::YaspGrid<3> grid(L,N,B,0);
      grid.globalRefine(3);

      // get view
      typedef Dune::YaspGrid<3>::LeafGridView GV;
      const GV& gv=grid.leafView(); 

      // make finite element map
      typedef GV::Grid::ctype DF;
      typedef Dune::PDELab::Q1LocalFiniteElementMap<DF,double,3> FEM;
      FEM fem;
      BCTypeParam bctype;
  
      // solve problem
      typedef Dune::PDELab::ConformingDirichletConstraints Constraints;
      poisson<GV,FEM,BCTypeParam,Constraints,2>(gv,fem,"poisson_yasp_Q1_3d",bctype);
    }
#endif

#if HAVE_UG
    {
#ifdef HANGING_NODES_REFINEMENT
      std::cout 
        << std::endl << std::endl
        << "Testcase 7.) UG 2D triangular cells (hanging nodes refinement) - P1 elements" 
        << std::endl;
#else
      std::cout 
        << std::endl << std::endl
        << "Alternative Testcase 7.) UG 2D triangular cells (uniform refinement) - P1 elements" 
        << std::endl;
#endif

      // make grid 
      typedef UGUnitSquare Grid;
      Grid grid;
      grid.setRefinementType( Grid::LOCAL );
      grid.setClosureType( Grid::NONE );  // This is needed to get hanging nodes refinement! Otherwise you would get triangles.
      grid.globalRefine(4);

      typedef UGUnitSquare::Codim<0>::Partition<Dune::All_Partition>::LeafIterator Iterator;
      typedef UGUnitSquare::LeafIntersectionIterator IntersectionIterator;
      typedef UGUnitSquare::LeafGridView GV;
      typedef UGUnitSquare::ctype ctype;

      // get view
      const GV& gv=grid.leafView(); 
 
#ifdef HANGING_NODES_REFINEMENT
      doSomeRandomRefinement<Grid,Iterator>( grid );
#endif

      // make finite element map
      typedef GV::Grid::ctype DF;
      typedef double R;
      const int k=1; //k=3;
      const int q=2*k;
      typedef Dune::PDELab::Pk2DLocalFiniteElementMap<GV,DF,double,k> FEM;
      FEM fem(gv);
  
      // We need the boundary function for the hanging nodes
      // constraints engine as we have to distinguish between hanging
      // nodes on dirichlet and on neumann boundaries
      BCTypeParam bctype;

#ifdef HANGING_NODES_REFINEMENT
      typedef Dune::PDELab::HangingNodesConstraintsAssemblers::SimplexGridP1Assembler ConstraintsAssembler;
      typedef Dune::PDELab::HangingNodesDirichletConstraints
        <GV::Grid,ConstraintsAssembler,BCTypeParam> Constraints;
      Constraints constraints(grid,true,bctype);
      poisson<GV,FEM,BCTypeParam,Constraints,q>( gv,
                                                 fem,
                                                 "poisson_UG_Pk_2d_hangingNodes",
                                                 bctype,
                                                 constraints 
                                                 );
#else
      typedef Dune::PDELab::ConformingDirichletConstraints Constraints;
      poisson<GV,FEM,BCTypeParam,Constraints,q>(gv,fem,"poisson_UG_Pk_2d",bctype);
#endif

    }
#endif // HAVE_UG





#if HAVE_UG
    {
#ifdef HANGING_NODES_REFINEMENT
      std::cout 
        << std::endl << std::endl
        << "Testcase 8.) UG 2D rectangular cells (hanging nodes refinement) - Q1 elements" 
        << std::endl;
#else
      std::cout 
        << std::endl << std::endl
        << "Alternative Testcase 8.) UG 2D rectangular cells (uniform refinement) - Q1 elements" 
        << std::endl;
#endif
      // make grid (unitcube made of cubes)
      typedef UGUnitSquareQ Grid;
      Grid grid;

      grid.setRefinementType( Grid::LOCAL );
      grid.setClosureType( Grid::NONE );  // This is needed to get hanging nodes refinement! Otherwise you would get triangles.
      grid.globalRefine(4);

      typedef UGUnitSquareQ::Codim<0>::Partition<Dune::All_Partition>::LeafIterator Iterator;
      typedef UGUnitSquareQ::LeafIntersectionIterator IntersectionIterator;
      typedef UGUnitSquareQ::LeafGridView GV;
      typedef UGUnitSquareQ::ctype ctype;
      
      // get view
      const GV& gv=grid.leafView();

#ifdef HANGING_NODES_REFINEMENT
      doSomeRandomRefinement<Grid,Iterator>( grid );
#endif

      // make finite element map
      typedef GV::Grid::ctype DF;
      typedef double R;
      typedef Dune::PDELab::Q1LocalFiniteElementMap<DF,R,2> FEM;
      FEM fem;

      BCTypeParam bctype;

#ifdef HANGING_NODES_REFINEMENT
      typedef Dune::PDELab::HangingNodesConstraintsAssemblers::CubeGridQ1Assembler ConstraintsAssembler;
      typedef Dune::PDELab::HangingNodesDirichletConstraints
        <GV::Grid,ConstraintsAssembler,BCTypeParam> Constraints;
      Constraints constraints(grid,true,bctype);
      poisson<GV,FEM,BCTypeParam,Constraints,2>( gv,
                                                 fem,
                                                 "poisson_UG_Q1_2d_hangingNodes",
                                                 bctype,
                                                 constraints
                                                 );
#else
      typedef Dune::PDELab::ConformingDirichletConstraints Constraints;
      poisson<GV,FEM,BCTypeParam,Constraints,2>(gv,fem,"poisson_UG_Q1_2d",bctype);
#endif

    }
#endif // HAVE_UG






#ifdef HAVE_UG
    {
#ifdef HANGING_NODES_REFINEMENT
      std::cout 
        << std::endl << std::endl
        << "Testcase 9.) UG 3D cubical cells (hanging nodes refinement) - Q1 elements" 
        << std::endl;
#endif
      // get grid and do a single global refine
      typedef UGUnitCube<3,1>::GridType Grid;
      UGUnitCube<3,1> ugunitcube;
      Grid & grid = ugunitcube.grid();
      grid.setRefinementType(Grid::LOCAL);
      grid.setClosureType(Grid::NONE);
      grid.globalRefine(1);

      typedef Grid::Codim<0>::Partition<Dune::All_Partition>::LeafIterator 
        Iterator;
      typedef Grid::LeafIntersectionIterator IntersectionIterator;
      typedef Grid::LeafGridView GV;
      typedef Grid::ctype ctype;
      
      // get view
      const GV& gv=grid.leafView(); 

#ifdef HANGING_NODES_REFINEMENT
      doSomeRandomRefinement<Grid,Iterator>( grid );
#endif

      // make finite element map
      typedef GV::Grid::ctype DF;
      typedef double R;
      const int q=2;
      typedef Dune::PDELab::Q1LocalFiniteElementMap<DF,R,3> FEM;
      FEM fem;

      BCTypeParam bctype;

#ifdef HANGING_NODES_REFINEMENT
      typedef Dune::PDELab::HangingNodesConstraintsAssemblers::CubeGridQ1Assembler ConstraintsAssembler;

      typedef Dune::PDELab::HangingNodesDirichletConstraints
        <GV::Grid,ConstraintsAssembler,BCTypeParam> Constraints;

      Constraints constraints(grid,true,bctype);
      poisson<GV,FEM,BCTypeParam,Constraints,q>( gv,
                                                 fem,
                                                 "poisson_UG_Q1_3d_hangingNodes",
                                                 bctype,
                                                 constraints
                                                 );
#endif

    }
#endif// HAVE_UG
      





#ifdef HAVE_UG
    {
#ifdef HANGING_NODES_REFINEMENT
      std::cout 
        << std::endl << std::endl
        << "Testcase 10.) UG 3D tetrahedral cells (hanging nodes refinement) - P1 elements" 
        << std::endl;
#else
      std::cout 
        << std::endl << std::endl
        << "Alternative Testcase 10.) UG 3D tetrahedral cells (uniform refinement) - P1 elements" 
        << std::endl;
#endif

      // UG Grid made of tetrahedrons - test Pk3D with hanging nodes!
      typedef UGUnitCube<3,2>::GridType Grid;
      UGUnitCube<3,2> ugunitcube;  
      Grid & grid = ugunitcube.grid();
      grid.setRefinementType(Grid::LOCAL);
      grid.setClosureType(Grid::NONE);
      grid.globalRefine(1);

      typedef Grid::Codim<0>::Partition<Dune::All_Partition>::LeafIterator Iterator;
      typedef Grid::LeafIntersectionIterator IntersectionIterator;
      typedef Grid::LeafGridView GV;
      typedef Grid::ctype ctype;
      
      // get view
      const GV& gv=grid.leafView(); 

#ifdef HANGING_NODES_REFINEMENT
      doSomeRandomRefinement<Grid,Iterator>( grid );
#endif
 
      // make finite element map
      typedef GV::Grid::ctype DF;
      typedef double R;
      const int k=1;     // polynomial degree
      const int q=2*k;
      typedef Dune::PDELab::Pk3DLocalFiniteElementMap<GV,DF,R,k> FEM;
      FEM fem(gv);
      BCTypeParam bctype;

#ifdef HANGING_NODES_REFINEMENT
      typedef Dune::PDELab::HangingNodesConstraintsAssemblers::SimplexGridP1Assembler ConstraintsAssembler;
      typedef Dune::PDELab::HangingNodesDirichletConstraints
        <GV::Grid,ConstraintsAssembler,BCTypeParam> Constraints;
      Constraints constraints(grid,true,bctype);
      poisson<GV,FEM,BCTypeParam,Constraints,q>( gv,
                                                 fem,
                                                 "poisson_UG_Pk_3d_hangingNodes",
                                                 bctype,
                                                 constraints
                                                 );
#else
      typedef Dune::PDELab::ConformingDirichletConstraints Constraints;
      poisson<GV,FEM,BCTypeParam,Constraints,q>(gv,fem,"poisson_UG_Pk_3d",bctype);
#endif

    }
#endif // HAVE_UG








#if HAVE_ALBERTA
    {
      std::cout 
        << std::endl << std::endl
        << "Testcase 11.) Alberta 2D triangular cells (uniform refinement) - Pk elements"
        << std::endl;

      // make grid 
      AlbertaUnitSquare grid;
      grid.globalRefine(8);
      
      // get view
      typedef AlbertaUnitSquare::LeafGridView GV;
      const GV& gv=grid.leafView(); 
      
      // make finite element map
      typedef GV::Grid::ctype DF;
      typedef double R;
      const int k=3;
      const int q=2*k;
      typedef Dune::PDELab::Pk2DLocalFiniteElementMap<GV,DF,double,k> FEM;
      FEM fem(gv);
      BCTypeParam bctype;
      
      // solve problem
      typedef Dune::PDELab::ConformingDirichletConstraints Constraints;
      poisson<GV,FEM,BCTypeParam,Constraints,q>(gv,fem,"poisson_Alberta_Pk_2d",bctype);
    }
#endif


	// test passed
	return 0;
  }
  catch (Dune::Exception &e){
    std::cerr << "Dune reported error: " << e << std::endl;
	return 1;
  }
  catch (...){
    std::cerr << "Unknown exception thrown!" << std::endl;
	return 1;
  }
}
