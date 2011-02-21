#include<dune/grid/io/file/vtk/subsamplingvtkwriter.hh>
#include<dune/pdelab/common/vtkexport.hh>
#include<dune/pdelab/gridfunctionspace/gridfunctionspace.hh>
#include<dune/pdelab/gridfunctionspace/gridfunctionspaceutilities.hh>
#include<dune/pdelab/gridfunctionspace/interpolate.hh>
#include<dune/pdelab/gridfunctionspace/constraints.hh>

template<typename GV>
void q1interpolate (const GV& gv)
{
  typedef typename GV::Grid::ctype D;// domain type
  typedef double R;                  // range type

  Q1LocalFiniteElementMap<D,R> fem;  // map entity to finite element

  typedef Dune::PDELab::GridFunctionSpace<GV,
	Q1LocalFiniteElementMap<D,R>,Q1Constraints> GFS;                    /*@\label{cint:newparameter}@*/
  GFS gfs(gv,fem);                   // make grid function space

  typedef typename GFS::template ConstraintsContainer<R>::Type T;       /*@\label{cint:container}@*/ 
  T t;                               // container for transformation    
  B<GV> b(gv);                       // boundary condition function     /*@\label{cint:bctfunction}@*/
  Dune::PDELab::constraints(b,gfs,t);// fill container                  /*@\label{cint:constraints}@*/

  typedef typename GFS::template VectorContainer<R>::Type X;
  X x(gfs,0.0);                      // make coefficient vector

  U<GV,R> u(gv);                     // analytic function object
  Dune::PDELab::interpolate(u,gfs,x);// interpolate x from u            
  Dune::PDELab::set_nonconstrained_dofs(t,0.0,x); // clear interior     /*@\label{cint:setconstraints}@*/

  typedef Dune::PDELab::DiscreteGridFunction<GFS,X> DGF;
  DGF dgf(gfs,x);                     // make a grid function

  Dune::SubsamplingVTKWriter<GV> vtkwriter(gv,1); // plot result
  vtkwriter.addVertexData(new Dune::PDELab::
						  VTKGridFunctionAdapter<DGF>(dgf,"q1"));
  vtkwriter.write("q1constrainedinterpolate",Dune::VTKOptions::ascii); 
}