#include "PHTpcTrackSeedCircleFit.h"

#include "AssocInfoContainer.h"

/// Tracking includes
#include <trackbase/TrkrDefs.h>                // for cluskey, getTrkrId, tpcId
#include <trackbase/TrkrClusterContainer.h>
#include <trackbase/TrkrCluster.h>
#include <trackbase_historic/SvtxTrack_v2.h>
#include <trackbase_historic/SvtxTrackMap.h>

#include <trackbase_historic/ActsTransformations.h>

#include <g4main/PHG4Hit.h>  // for PHG4Hit
#include <g4main/PHG4Particle.h>  // for PHG4Particle
#include <g4main/PHG4HitDefs.h>  // for keytype

#include <fun4all/Fun4AllReturnCodes.h>

#include <phool/getClass.h>
#include <phool/phool.h>

#if __cplusplus < 201402L
#include <boost/make_unique.hpp>
#endif

#include <TF1.h>

#include <climits>                            // for UINT_MAX
#include <iostream>                            // for operator<<, basic_ostream
#include <cmath>                              // for fabs, sqrt
#include <set>                                 // for _Rb_tree_const_iterator
#include <utility>                             // for pair
#include <memory>
using namespace std;

//____________________________________________________________________________..
PHTpcTrackSeedCircleFit::PHTpcTrackSeedCircleFit(const std::string &name):
 SubsysReco(name)
 , _track_map_name_silicon("SvtxSiliconTrackMap")
{
  //cout << "PHTpcTrackSeedCircleFit::PHTpcTrackSeedCircleFit(const std::string &name) Calling ctor" << endl;
}

//____________________________________________________________________________..
PHTpcTrackSeedCircleFit::~PHTpcTrackSeedCircleFit()
{

}

//____________________________________________________________________________..
int PHTpcTrackSeedCircleFit::InitRun(PHCompositeNode *topNode)
{
  //  std::cout << PHWHERE << " Parameters: _reject_xy_outliers " << _reject_xy_outliers << " _xy_residual_cut " << _xy_residual_cut
  //	    << " _reject_z_outliers " << _reject_z_outliers << " _z_residual_cut " << _z_residual_cut  << " _refit " << _refit
  //	    << std::endl; 

  int ret = GetNodes(topNode);

  return ret;
}

//____________________________________________________________________________..
int PHTpcTrackSeedCircleFit::process_event(PHCompositeNode *)
{
  // _track_map contains the TPC seed track stubs
  // We want to associate these TPC track seeds with a collision vertex
  // All we need is to project the TPC clusters in Z to the beam line.
  // The TPC track seeds are given a position that is the PCA of the line and circle fit to the beam line

  if(Verbosity() > 0)
    cout << PHWHERE << " TPC track map size " << _track_map->size()  << endl;

  // loop over the TPC track seeds
  for (auto phtrk_iter = _track_map->begin();
       phtrk_iter != _track_map->end(); 
       ++phtrk_iter)
    {
      _tracklet_tpc = phtrk_iter->second;
      
      if (Verbosity() > 1)
	{
	  std::cout
	    << __LINE__
	    << ": Processing seed itrack: " << phtrk_iter->first
	    << ": nhits: " << _tracklet_tpc-> size_cluster_keys()
	    << ": pT: " << _tracklet_tpc->get_pt()
	    << ": phi: " << _tracklet_tpc->get_phi()
	    << ": eta: " << _tracklet_tpc->get_eta()
	    << endl;
	}

      // get the tpc track seed cluster positions in z and r

      // Get the TPC clusters for this tracklet
      std::vector<TrkrCluster*> clusters = getTrackClusters(_tracklet_tpc);

      // count TPC layers for this track
      std::set<unsigned int> layers;
      for (unsigned int i=0; i<clusters.size(); ++i)
	{
	  unsigned int layer = TrkrDefs::getLayer(clusters[i]->getClusKey());
	  layers.insert(layer);
	}
      unsigned int nlayers = layers.size();
      if(Verbosity() > 2) std::cout << "    TPC layers this track: " << nlayers << std::endl;

      ActsTransformations transformer;

      std::vector<std::pair<double, double>> cpoints;
      std::vector<Acts::Vector3D> globalClusterPositions;
      for (unsigned int i=0; i<clusters.size(); ++i)
	{
	  auto global = transformer.getGlobalPosition(clusters.at(i),
						       _surfmaps,
						       _tGeometry);
	  globalClusterPositions.push_back(global);
	  cpoints.push_back(make_pair(global(0), global(1)));
	}
      
      if(clusters.size() < 3)
	{
	  if(Verbosity() > 3) std::cout << PHWHERE << "  -- skip this tpc tracklet, not enough TPC clusters " << std::endl; 
	  continue;  // skip to the next TPC tracklet
	}

      // get the straight line representing the z trajectory in the form of z vs radius
      double A = 0; double B = 0;
      line_fit_clusters(globalClusterPositions, A, B);
      if(Verbosity() > 2) std::cout << " First fitted line has A " << A << " B " << B << std::endl;

      // Project this TPC tracklet  to the beam line and store the projections
      _z_proj = B;
      
      // set the track Z position to the Z dca
      _tracklet_tpc->set_z(_z_proj);

      // Finished association of track with vertex, now we modify the track parameters

      // extract the track theta
      double track_angle = atan(A);  // referenced to 90 degrees
  
      
      // make circle fit
      double R, X0, Y0;
      CircleFitByTaubin(cpoints, R, X0, Y0);
      if(Verbosity() > 2) 
	std::cout << " Fitted circle has R " << R << " X0 " << X0 << " Y0 " << Y0 << std::endl;

      // set the track x and y positions to the circle PCA
      double dcax, dcay;
      findRoot(R, X0, Y0, dcax, dcay);
      _tracklet_tpc->set_x(dcax);
      _tracklet_tpc->set_y(dcay);
      
      double pt_track = _tracklet_tpc->get_pt();
      if(Verbosity() > 5)
	{
	  //  get the pT from radius of circle as a check 
	  double Bfield = 1.4;  // T
	  double pt_circle = 0.3 * Bfield * R * 0.01;  // convert cm to m 
	  std::cout << " pT from circle of radius R = " << R << " in field of " << Bfield << " Tesla is " << pt_circle << " seed pT is " << pt_track  << std::endl; 
	}
      
      // We want the angle of the tangent relative to the positive x axis
      // start with the angle of the radial line from vertex to circle center

      double dx = X0 - dcax;
      double dy = Y0 - dcay;
      double phi= atan2(-dx,dy);
    
      // convert to the angle of the tangent to the circle
      // we need to know if the track proceeds clockwise or CCW around the circle
      double dx0 = cpoints[0].first - X0;
      double dy0 = cpoints[0].second - Y0;
      double phi0 = atan2(dy0, dx0);
      double dx1 = cpoints[1].first - X0;
      double dy1 = cpoints[1].second - Y0;
      double phi1 = atan2(dy1, dx1);
      double dphi = phi1 - phi0;

      // need to deal with the switch from -pi to +pi at phi = 180 degrees
      // final phi - initial phi must be < 180 degrees for it to be a valid track
      if(dphi > M_PI) dphi -= 2.0 * M_PI;
      if(dphi < - M_PI) dphi += M_PI;

      if(Verbosity() > 5) 
	{
	  std::cout << " charge " <<  _tracklet_tpc->get_charge() << " phi0 " << phi0*180.0 / M_PI << " phi1 " << phi1*180.0 / M_PI 
		    << " dphi " << dphi*180.0 / M_PI << std::endl;
	}

      // whether we add 180 degrees depends on the angle of the bend
      if(dphi < 0)
	{ 
	  phi += M_PI; 
	  if(phi > M_PI)
	    { phi -= 2. * M_PI; }
	}

      if(Verbosity() > 5) 
	std::cout << " input track phi " << _tracklet_tpc->get_phi()  << " new phi " << phi  << std::endl;  

      // get the updated values of px, py, pz from the pT and the angles found here
      double px_new = pt_track * cos(phi);
      double py_new = pt_track * sin(phi);
      double ptrack_new = pt_track / cos(track_angle);
      double pz_new = ptrack_new * sin(track_angle);

      if(Verbosity() > 5)
	std::cout << " input track mom " << _tracklet_tpc->get_p() << " new mom " << ptrack_new
		  << " px in " << _tracklet_tpc->get_px()  << " px " << px_new 
		  << " py in " << _tracklet_tpc->get_py() << " py " << py_new 
		  << " pz in " << _tracklet_tpc->get_pz() << " pz " << pz_new 
		  << " eta in " <<  _tracklet_tpc->get_eta() << " phi in " <<  _tracklet_tpc->get_phi() * 180.0 / M_PI
		  << " track angle " << track_angle * 180.0 / M_PI 
		  << std::endl;

      // update track on node tree
      _tracklet_tpc->set_px(px_new);
      _tracklet_tpc->set_py(py_new);
      _tracklet_tpc->set_pz(pz_new);

      if(Verbosity() > 5)
	std::cout << " new mom " <<  _tracklet_tpc->get_p() <<  "  new eta " <<  _tracklet_tpc->get_eta() 
		  << " new phi " << _tracklet_tpc->get_phi() * 180.0 / M_PI << std::endl;
      
    }  // end loop over TPC track seeds

   if(Verbosity() > 0)  
    cout << " Final track map size " << _track_map->size() << endl;
  
  if (Verbosity() > 0)
    cout << "PHTpcTrackSeedCircleFit::process_event(PHCompositeNode *topNode) Leaving process_event" << endl;  
  
  return Fun4AllReturnCodes::EVENT_OK;
}

int PHTpcTrackSeedCircleFit::End(PHCompositeNode*)
{
  return Fun4AllReturnCodes::EVENT_OK;
}

int  PHTpcTrackSeedCircleFit::GetNodes(PHCompositeNode* topNode)
{
  _surfmaps = findNode::getClass<ActsSurfaceMaps>(topNode,"ActsSurfaceMaps");
  if(!_surfmaps)
    {
      std::cout << PHWHERE << "Error, can't find acts surface maps" << std::endl;
      return Fun4AllReturnCodes::ABORTEVENT;
    }

  _tGeometry = findNode::getClass<ActsTrackingGeometry>(topNode,"ActsTrackingGeometry");
  if(!_tGeometry)
    {
      std::cout << PHWHERE << "Error, can't find acts tracking geometry" << std::endl;
      return Fun4AllReturnCodes::ABORTEVENT;
    }

  if(_use_truth_clusters)
    _cluster_map = findNode::getClass<TrkrClusterContainer>(topNode, "TRKR_CLUSTER_TRUTH");
  else
    _cluster_map = findNode::getClass<TrkrClusterContainer>(topNode, "TRKR_CLUSTER");

  if (!_cluster_map)
  {
    cerr << PHWHERE << " ERROR: Can't find node TRKR_CLUSTER" << endl;
    return Fun4AllReturnCodes::ABORTEVENT;
  }

  _track_map = findNode::getClass<SvtxTrackMap>(topNode, _track_map_name);
  if (!_track_map)
  {
    cerr << PHWHERE << " ERROR: Can't find SvtxTrackMap" << endl;
    return Fun4AllReturnCodes::ABORTEVENT;
  }

  return Fun4AllReturnCodes::EVENT_OK;
}


void  PHTpcTrackSeedCircleFit::line_fit(std::vector<std::pair<double,double>> points, double &a, double &b)
{
  // copied from: https://www.bragitoff.com
  // we want to fit z vs radius
  
   double xsum=0,x2sum=0,ysum=0,xysum=0;                //variables for sums/sigma of xi,yi,xi^2,xiyi etc
   for (unsigned int i=0; i<points.size(); ++i)
    {
      double r = points[i].first;
      double z = points[i].second;

      xsum=xsum+r;                        //calculate sigma(xi)
      ysum=ysum+z;                        //calculate sigma(yi)
      x2sum=x2sum+pow(r,2);                //calculate sigma(x^2i)
      xysum=xysum+r*z;                    //calculate sigma(xi*yi)
    }
   a=(points.size()*xysum-xsum*ysum)/(points.size()*x2sum-xsum*xsum);            //calculate slope
   b=(x2sum*ysum-xsum*xysum)/(x2sum*points.size()-xsum*xsum);            //calculate intercept

   if(Verbosity() > 10)
     {
       for (unsigned int i=0;i<points.size(); ++i)
	 {
	   double r = points[i].first;
	   double z_fit = a * r + b;                    //to calculate z(fitted) at given r points
	   std::cout << " r " << r << " z " << points[i].second << " z_fit " << z_fit << std::endl; 
	 } 
     }

    return;
}   

void PHTpcTrackSeedCircleFit::line_fit_clusters(std::vector<Acts::Vector3D>& globPos, double &a, double &b)
{
  std::vector<std::pair<double,double>> points;
  
  for(auto& pos : globPos)
     {
       double z = pos(2);
       double r = sqrt(pow(pos(0),2) + pow(pos(1), 2));

       points.push_back(make_pair(r,z));
     }

   line_fit(points, a, b);

    return;
}

void PHTpcTrackSeedCircleFit::CircleFitByTaubin (std::vector<std::pair<double,double>> points, double &R, double &X0, double &Y0)
/*  
      Circle fit to a given set of data points (in 2D)
      This is an algebraic fit, due to Taubin, based on the journal article
      G. Taubin, "Estimation Of Planar Curves, Surfaces And Nonplanar
                  Space Curves Defined By Implicit Equations, With 
                  Applications To Edge And Range Image Segmentation",
                  IEEE Trans. PAMI, Vol. 13, pages 1115-1138, (1991)
*/
{
  int iter,IterMAX=99;
  
  double Mz,Mxy,Mxx,Myy,Mxz,Myz,Mzz,Cov_xy,Var_z;
  double A0,A1,A2,A22,A3,A33;
  double x,y;
  double DET,Xcenter,Ycenter;
  
  // Compute x- and y- sample means   
  double meanX = 0;
  double meanY = 0;
  double weight = 0;
  for(unsigned int i = 0; i < points.size(); ++i)
    {
      meanX += points[i].first;
      meanY += points[i].second;
      weight++;
    }
  meanX /= weight;
  meanY /= weight;

  //     computing moments 
  
  Mxx=Myy=Mxy=Mxz=Myz=Mzz=0.;
  
  for (unsigned int i=0; i<points.size(); i++)
    {
      double Xi = points[i].first - meanX;   //  centered x-coordinates
      double Yi = points[i].second - meanY;   //  centered y-coordinates
      double Zi = Xi*Xi + Yi*Yi;
      
      Mxy += Xi*Yi;
      Mxx += Xi*Xi;
      Myy += Yi*Yi;
      Mxz += Xi*Zi;
      Myz += Yi*Zi;
      Mzz += Zi*Zi;
    }
  Mxx /= weight;
  Myy /= weight;
  Mxy /= weight;
  Mxz /= weight;
  Myz /= weight;
  Mzz /= weight;
  
  //  computing coefficients of the characteristic polynomial
  
  Mz = Mxx + Myy;
  Cov_xy = Mxx*Myy - Mxy*Mxy;
  Var_z = Mzz - Mz*Mz;
  A3 = 4*Mz;
  A2 = -3*Mz*Mz - Mzz;
  A1 = Var_z*Mz + 4*Cov_xy*Mz - Mxz*Mxz - Myz*Myz;
  A0 = Mxz*(Mxz*Myy - Myz*Mxy) + Myz*(Myz*Mxx - Mxz*Mxy) - Var_z*Cov_xy;
  A22 = A2 + A2;
  A33 = A3 + A3 + A3;
  
  //    finding the root of the characteristic polynomial
  //    using Newton's method starting at x=0  
  //    (it is guaranteed to converge to the right root)
  
  for (x=0.,y=A0,iter=0; iter<IterMAX; iter++)  // usually, 4-6 iterations are enough
    {
      double Dy = A1 + x*(A22 + A33*x);
      double xnew = x - y/Dy;
      if ((xnew == x)||(!isfinite(xnew))) break;
      double ynew = A0 + xnew*(A1 + xnew*(A2 + xnew*A3));
      if (fabs(ynew)>=fabs(y))  break;
      x = xnew;  y = ynew;
    }
  
  //  computing parameters of the fitting circle
  
  DET = x*x - x*Mz + Cov_xy;
  Xcenter = (Mxz*(Myy - x) - Myz*Mxy)/DET/2;
  Ycenter = (Myz*(Mxx - x) - Mxz*Mxy)/DET/2;
  
  //  assembling the output
  
  X0 = Xcenter + meanX;
  Y0 = Ycenter + meanY;
  R = sqrt(Xcenter*Xcenter + Ycenter*Ycenter + Mz);
}

std::vector<double> PHTpcTrackSeedCircleFit::GetCircleClusterResiduals(std::vector<std::pair<double,double>> points, double R, double X0, double Y0)
{
  std::vector<double> residues;
  // calculate cluster residuals from the fitted circle
  for(unsigned int i = 0; i < points.size(); ++i)
    {
      double x = points[i].first;
      double y = points[i].second;

      // The shortest distance of a point from a circle is along the radial; line from the circle center to the point
      double dca = sqrt( (x - X0)*(x-X0) + (y-Y0)*(y-Y0) )  -  R;  
      residues.push_back(dca);
    }
  return residues;  
}

std::vector<double> PHTpcTrackSeedCircleFit::GetLineClusterResiduals(std::vector<std::pair<double,double>> points, double A, double B)
{
  std::vector<double> residues;
  // calculate cluster residuals from the fitted circle
  for(unsigned int i = 0; i < points.size(); ++i)
    {
      double r = points[i].first;
      double z = points[i].second;

      // The shortest distance of a point from a circle is along the radial; line from the circle center to the point

      double a = -A;
      double b = 1.0;
      double c = -B;
      double dca = sqrt(pow(a*r+b*z+c, 2)) / sqrt(a*a+b*b);

      residues.push_back(dca);
    }
  return residues;  
}

std::vector<TrkrCluster*> PHTpcTrackSeedCircleFit::getTrackClusters(SvtxTrack *_tracklet_tpc)
{
  std::vector<TrkrCluster*> clusters;
  
  for (SvtxTrack::ConstClusterKeyIter key_iter = _tracklet_tpc->begin_cluster_keys();
       key_iter != _tracklet_tpc->end_cluster_keys();
       ++key_iter)
    {
      TrkrDefs::cluskey cluster_key = *key_iter;
      unsigned int layer = TrkrDefs::getLayer(cluster_key);
      
      if(layer < _min_tpc_layer) continue;
      if(layer > _max_tpc_layer) continue;
      
      // get the cluster
      TrkrCluster *tpc_clus =  _cluster_map->findCluster(cluster_key);
      
      //tpc_clusters_map.insert(std::make_pair(layer, tpc_clus));
      clusters.push_back(tpc_clus);
      
      if(Verbosity() > 5) 
	std::cout << "  TPC cluster in layer " << layer << " with local position " << tpc_clus->getLocalX() 
		  << "  " << tpc_clus->getLocalY() << " clusters.size() " << clusters.size() << std::endl;
    }

  return clusters;
}

void PHTpcTrackSeedCircleFit::findRoot(const double R, const double X0,
				    const double Y0, double& x,
				    double& y)
{
  /**
   * We need to determine the closest point on the circle to the origin
   * since we can't assume that the track originates from the origin
   * The eqn for the circle is (x-X0)^2+(y-Y0)^2=R^2 and we want to 
   * minimize d = sqrt((0-x)^2+(0-y)^2), the distance between the 
   * origin and some (currently, unknown) point on the circle x,y.
   * 
   * Solving the circle eqn for x and substituting into d gives an eqn for
   * y. Taking the derivative and setting equal to 0 gives the following 
   * two solutions. We take the smaller solution as the correct one, as 
   * usually one solution is wildly incorrect (e.g. 1000 cm)
   */
  
  double miny = (sqrt(pow(X0, 2) * pow(R, 2) * pow(Y0, 2) + pow(R, 2) 
		      * pow(Y0,4)) + pow(X0,2) * Y0 + pow(Y0, 3)) 
    / (pow(X0, 2) + pow(Y0, 2));

  double miny2 = (-sqrt(pow(X0, 2) * pow(R, 2) * pow(Y0, 2) + pow(R, 2) 
		      * pow(Y0,4)) + pow(X0,2) * Y0 + pow(Y0, 3)) 
    / (pow(X0, 2) + pow(Y0, 2));

  double minx = sqrt(pow(R, 2) - pow(miny - Y0, 2)) + X0;
  double minx2 = -sqrt(pow(R, 2) - pow(miny2 - Y0, 2)) + X0;
  
  if(Verbosity() > 1)
    std::cout << "minx1 and x2 : " << minx << ", " << minx2 << std::endl
	      << "miny1 and y2 : " << miny << ", " << miny2 << std::endl;

  /// Figure out which of the two roots is actually closer to the origin
  if(fabs(minx) < fabs(minx2))
    x = minx;
  else
    x = minx2;

  if(fabs(miny) < fabs(miny2))
    y = miny;
  else
    y = miny2;
  
  if(Verbosity() > 1)
    {
      std::cout << "Minimum x and y positions " << x << ",  " 
		<< y << std::endl;
    }

}
