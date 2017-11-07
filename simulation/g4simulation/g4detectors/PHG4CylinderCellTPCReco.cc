#include "PHG4CylinderCellTPCReco.h"
#include "PHG4CellContainer.h"
#include "PHG4CellDefs.h"
#include "PHG4Cellv1.h"
#include "PHG4CylinderCellGeom.h"
#include "PHG4CylinderCellGeomContainer.h"
#include "PHG4CylinderGeom.h"
#include "PHG4CylinderGeomContainer.h"
#include "PHG4TPCDistortion.h"

#include <g4main/PHG4Hit.h>
#include <g4main/PHG4HitContainer.h>

#include <fun4all/Fun4AllReturnCodes.h>
#include <fun4all/Fun4AllServer.h>

#include <phool/PHCompositeNode.h>
#include <phool/PHIODataNode.h>
#include <phool/PHNodeIterator.h>
#include <phool/PHRandomSeed.h>
#include <phool/getClass.h>

#include <TH1F.h>
#include <TProfile2D.h>
#include <TROOT.h>
#include <TSystem.h>

#include <CLHEP/Units/PhysicalConstants.h>
#include <CLHEP/Units/SystemOfUnits.h>

#include <gsl/gsl_randist.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>

using namespace std;

PHG4CylinderCellTPCReco::PHG4CylinderCellTPCReco(int n_pixel,
                                                 const string &name)
  : SubsysReco(name)
  , _timer(PHTimeServer::get()->insert_new(name))
  , fHalfLength(100)
  , fDiffusionT(0.0057)
  , fDiffusionL(0.0057)
  , elec_per_gev(38. * 1e6)
  , driftv(3.0 / 1000.0)
  ,  // cm per ns
  num_pixel_layers(n_pixel)
  , tmin_default(0.0)
  ,  // ns
  tmax_default(60.0)
  ,  // ns
  tmin_max()
  , distortion(NULL)
  , fHElectrons(NULL)
  , fHWindowP(NULL)
  , fHWindowZ(NULL)
  , fHMeanEDepPerCell(NULL)
  , fHMeanElectronsPerCell(NULL)
  , fHErrorRPhi(NULL)
  , fHErrorZ(NULL)
  , fFractRPsm(0.0)
  , fFractZZsm(0.0)
  , fShapingLead(32.0 * 3.0 / 1000.0)
  ,                                  // ns
  fShapingTail(48.0 * 3.0 / 1000.0)  // ns
{
  memset(nbins, 0, sizeof(nbins));
  unsigned int seed = PHRandomSeed();  // fixed seed is handled in this funtcion
  cout << Name() << " random seed: " << seed << endl;
  RandomGenerator = gsl_rng_alloc(gsl_rng_mt19937);
  gsl_rng_set(RandomGenerator, seed);
}

PHG4CylinderCellTPCReco::~PHG4CylinderCellTPCReco()
{
  gsl_rng_free(RandomGenerator);
  delete distortion;
}

void PHG4CylinderCellTPCReco::Detector(const std::string &d)
{
  detector = d;
  // only set the outdetector nodename if it wasn't set already
  // in case the order in the macro is outdetector(); detector();
  if (outdetector.size() == 0)
  {
    OutputDetector(d);
  }
  return;
}

void PHG4CylinderCellTPCReco::cellsize(const int i, const double sr, const double sz)
{
  cell_size[i] = std::make_pair(sr, sz);
}

int PHG4CylinderCellTPCReco::Init(PHCompositeNode *top_node)
{
  if (verbosity > 1)
  {
    Fun4AllServer *se = Fun4AllServer::instance();
    fHWindowP = new TProfile2D("TPCGEO_WindowP", "TPCGEO_WindowP", 50, -0.5, 49.5, 220, -110, +110);
    se->registerHisto(fHWindowP);
    fHWindowZ = new TProfile2D("TPCGEO_WindowZ", "TPCGEO_WindowZ", 50, -0.5, 49.5, 220, -110, +110);
    se->registerHisto(fHWindowZ);
    fHElectrons = new TH1F("TPCGEO_GeneratedElectrons", "TPCGEO_GeneratedElectrons", 100, 0, 200);
    se->registerHisto(fHElectrons);
    fHMeanEDepPerCell = new TProfile2D("TPCGEO_Edep", "TPCGEO_Edep", 50, -0.5, 49.5, 220, -110, +110);
    se->registerHisto(fHMeanEDepPerCell);
    fHMeanElectronsPerCell = new TProfile2D("TPCGEO_Electrons", "TPCGEO_Electrons", 50, -0.5, 49.5, 220, -110, +110);
    se->registerHisto(fHMeanElectronsPerCell);
    fHErrorRPhi = new TProfile2D("TPCGEO_CloudSizeRPhi", "TPCGEO_CloudSizeRPhi", 50, -0.5, 49.5, 220, -110, +110);
    se->registerHisto(fHErrorRPhi);
    fHErrorZ = new TProfile2D("TPCGEO_CloudSizeZ", "TPCGEO_CloudSizeZ", 50, -0.5, 49.5, 220, -110, +110);
    se->registerHisto(fHErrorZ);
  }

  return Fun4AllReturnCodes::EVENT_OK;
}

int PHG4CylinderCellTPCReco::InitRun(PHCompositeNode *topNode)
{
  PHNodeIterator iter(topNode);

  PHCompositeNode *dstNode;
  dstNode = dynamic_cast<PHCompositeNode *>(iter.findFirst("PHCompositeNode", "DST"));
  if (!dstNode)
  {
    std::cout << PHWHERE << "DST Node missing, doing nothing." << std::endl;
    exit(1);
  }
  hitnodename = "G4HIT_" + detector;
  PHG4HitContainer *g4hit = findNode::getClass<PHG4HitContainer>(topNode, hitnodename.c_str());
  if (!g4hit)
  {
    cout << "Could not locate g4 hit node " << hitnodename << endl;
    exit(1);
  }
  cellnodename = "G4CELL_" + outdetector;
  PHG4CellContainer *cells = findNode::getClass<PHG4CellContainer>(topNode, cellnodename);
  if (!cells)
  {
    cells = new PHG4CellContainer();
    PHIODataNode<PHObject> *newNode = new PHIODataNode<PHObject>(cells, cellnodename.c_str(), "PHObject");
    dstNode->addNode(newNode);
  }
  geonodename = "CYLINDERGEOM_" + detector;
  PHG4CylinderGeomContainer *geo = findNode::getClass<PHG4CylinderGeomContainer>(topNode, geonodename.c_str());
  if (!geo)
  {
    cout << "Could not locate geometry node " << geonodename << endl;
    exit(1);
  }
  seggeonodename = "CYLINDERCELLGEOM_" + outdetector;
  PHG4CylinderCellGeomContainer *seggeo = findNode::getClass<PHG4CylinderCellGeomContainer>(topNode, seggeonodename.c_str());
  if (!seggeo)
  {
    seggeo = new PHG4CylinderCellGeomContainer();
    PHCompositeNode *runNode = dynamic_cast<PHCompositeNode *>(iter.findFirst("PHCompositeNode", "RUN"));
    PHIODataNode<PHObject> *newNode = new PHIODataNode<PHObject>(seggeo, seggeonodename.c_str(), "PHObject");
    runNode->addNode(newNode);
  }

  map<int, PHG4CylinderGeom *>::const_iterator miter;
  pair<map<int, PHG4CylinderGeom *>::const_iterator, map<int, PHG4CylinderGeom *>::const_iterator> begin_end = geo->get_begin_end();
  map<int, std::pair<double, double> >::iterator sizeiter;
  for (miter = begin_end.first; miter != begin_end.second; ++miter)
  {
    PHG4CylinderGeom *layergeom = miter->second;
    int layer = layergeom->get_layer();
    double circumference = layergeom->get_radius() * 2 * M_PI;
    double length_in_z = layergeom->get_zmax() - layergeom->get_zmin();
    sizeiter = cell_size.find(layer);
    if (sizeiter == cell_size.end())
    {
      cout << "no cell sizes for layer " << layer << endl;
      exit(1);
    }
    PHG4CylinderCellGeom *layerseggeo = new PHG4CylinderCellGeom();
    layerseggeo->set_layer(layergeom->get_layer());
    layerseggeo->set_radius(layergeom->get_radius());
    layerseggeo->set_thickness(layergeom->get_thickness());

    double size_z = (sizeiter->second).second;
    double size_r = (sizeiter->second).first;
    double bins_r;
    // unlikely but if the circumference is a multiple of the cell size
    // use result of division, if not - add 1 bin which makes the
    // cells a tiny bit smaller but makes them fit
    double fract = modf(circumference / size_r, &bins_r);
    if (fract != 0)
    {
      bins_r++;
    }
    nbins[0] = bins_r;
    size_r = circumference / bins_r;
    (sizeiter->second).first = size_r;
    double phistepsize = 2 * M_PI / bins_r;
    double phimin = -M_PI;
    double phimax = phimin + phistepsize;
    phistep[layer] = phistepsize;
    for (int i = 0; i < nbins[0]; i++)
    {
      phimax += phistepsize;
    }
    // unlikely but if the length is a multiple of the cell size
    // use result of division, if not - add 1 bin which makes the
    // cells a tiny bit smaller but makes them fit
    fract = modf(length_in_z / size_z, &bins_r);
    if (fract != 0)
    {
      bins_r++;
    }
    nbins[1] = bins_r;
    pair<int, int> phi_z_bin = make_pair(nbins[0], nbins[1]);
    n_phi_z_bins[layer] = phi_z_bin;
    // update our map with the new sizes
    size_z = length_in_z / bins_r;
    (sizeiter->second).second = size_z;
    double zlow = layergeom->get_zmin();
    double zhigh = zlow + size_z;
    ;
    for (int i = 0; i < nbins[1]; i++)
    {
      zhigh += size_z;
    }
    layerseggeo->set_binning(PHG4CellDefs::sizebinning);
    layerseggeo->set_zbins(nbins[1]);
    layerseggeo->set_zmin(layergeom->get_zmin());
    layerseggeo->set_zstep(size_z);
    layerseggeo->set_phibins(nbins[0]);
    layerseggeo->set_phistep(phistepsize);
    // Chris Pinkenburg: greater causes huge memory growth which causes problems
    // on our farm. If you need to increase this - TALK TO ME first
    if (nbins[1] * nbins[0] > 5100000)
    {
      cout << "increase TPC cellsize, number of cells "
           << nbins[1] * nbins[0] << " for layer " << layer
           << " exceed 5.1M limit" << endl;
      cout << "minimum working values for cells are  0.12/0.17" << endl;
      gSystem->Exit(1);
    }
    seggeo->AddLayerCellGeom(layerseggeo);
  }

  for (std::map<int, int>::iterator iter = binning.begin();
       iter != binning.end(); ++iter)
  {
    int layer = iter->first;
    // if the user doesn't set an integration window, set the default
    tmin_max.insert(std::make_pair(layer, std::make_pair(tmin_default, tmax_default)));
  }

  return Fun4AllReturnCodes::EVENT_OK;
}

int PHG4CylinderCellTPCReco::process_event(PHCompositeNode *topNode)
{
  _timer.get()->restart();
  PHG4HitContainer *g4hit = findNode::getClass<PHG4HitContainer>(topNode, hitnodename.c_str());
  if (!g4hit)
  {
    cout << "Could not locate g4 hit node " << hitnodename << endl;
    exit(1);
  }
  PHG4CellContainer *cells = findNode::getClass<PHG4CellContainer>(topNode, cellnodename);
  if (!cells)
  {
    cout << "could not locate cell node " << cellnodename << endl;
    exit(1);
  }
  PHG4CylinderCellGeomContainer *seggeo = findNode::getClass<PHG4CylinderCellGeomContainer>(topNode, seggeonodename.c_str());
  if (!seggeo)
  {
    cout << "could not locate geo node " << seggeonodename << endl;
    exit(1);
  }

  map<int, std::pair<double, double> >::iterator sizeiter;
  PHG4HitContainer::LayerIter layer;
  pair<PHG4HitContainer::LayerIter, PHG4HitContainer::LayerIter> layer_begin_end = g4hit->getLayers();
  for (layer = layer_begin_end.first; layer != layer_begin_end.second; layer++)
  {
    std::map<unsigned long long, PHG4Cell *> cellptmap;
    PHG4HitContainer::ConstIterator hiter;
    PHG4HitContainer::ConstRange hit_begin_end = g4hit->getHits(*layer);
    PHG4CylinderCellGeom *geo = seggeo->GetLayerCellGeom(*layer);
    if (verbosity > 1000)
    {
      std::cout << "Layer " << (*layer);
      std::cout << " Radius " << geo->get_radius();
      std::cout << " Thickness " << geo->get_thickness();
      std::cout << " zmin " << geo->get_zmin();
      std::cout << " nbinsz " << geo->get_zbins();
      std::cout << " phimin " << geo->get_phimin();
      std::cout << " nbinsphi " << geo->get_phibins();
      std::cout << std::endl;
    }

    int nphibins = n_phi_z_bins[*layer].first;
    int nzbins = n_phi_z_bins[*layer].second;

    sizeiter = cell_size.find(*layer);
    if (sizeiter == cell_size.end())
    {
      cout << "logical screwup!!! no sizes for layer " << *layer << endl;
      exit(1);
    }
    double zstepsize = (sizeiter->second).second;
    double phistepsize = phistep[*layer];
    for (hiter = hit_begin_end.first; hiter != hit_begin_end.second; hiter++)
    {
      // checking ADC timing integration window cut
      if (hiter->second->get_t(0) > tmin_max[*layer].second) continue;
      if (hiter->second->get_t(1) < tmin_max[*layer].first) continue;

      // the matching z-bin window in the corresponding cells
      int min_cell_zbin = 0;
      int max_cell_zbin = nzbins-1;
      if (hiter->second->get_avg_z()>0)
      {
        //positive drifting volume

        min_cell_zbin = nzbins/2;
      }
      else
      {
        //negative drifting volume

        max_cell_zbin = nzbins/2 - 1;
      }

      double xinout;
      double yinout;
      double phi;
      double z;
      int phibin;
      int zbin;
      xinout = hiter->second->get_avg_x();
      yinout = hiter->second->get_avg_y();
      double r = sqrt(xinout * xinout + yinout * yinout);
      phi = atan2(hiter->second->get_avg_y(), hiter->second->get_avg_x());
      z = hiter->second->get_avg_z();
      if (verbosity > 2000)
        cout << "loop over hits, hit avge z = " << hiter->second->get_avg_z()
             << " hit avge x = " << hiter->second->get_avg_x()
             << " hit avge y = " << hiter->second->get_avg_y()
             << " phi = " << phi
             << endl;

      // apply primary charge distortion
      if ((*layer) >= (unsigned int) num_pixel_layers)
      {  // in TPC
        if (distortion)
        {
          // do TPC distortion
          const double dz = distortion->get_z_distortion(r, phi, z);
          const double drphi = distortion->get_rphi_distortion(r, phi, z);
          //TODO: radial distortion is not applied at the moment,
          //      because it leads to major change to the structure of this code and it affect the insensitive direction to
          //      near radial tracks
          //
          //          const double dr = distortion ->get_r_distortion(r,phi,z);
          phi += drphi / r;
          z += dz;
        }
        //TODO: this is an approximation of average track propagation time correction on a cluster's hit time or z-position.
        // Full simulation require implement this correction in PHG4TPCClusterizer::process_event
        const double approximate_cluster_path_length = sqrt(
            hiter->second->get_avg_x() * hiter->second->get_avg_x() + hiter->second->get_avg_y() * hiter->second->get_avg_y() + hiter->second->get_avg_z() * hiter->second->get_avg_z());
        const double speed_of_light_cm_ns = CLHEP::c_light / (CLHEP::centimeter / CLHEP::nanosecond);
        if (z >= 0.0)
          z -= driftv * (hiter->second->get_avg_t() - approximate_cluster_path_length / speed_of_light_cm_ns);
        else
          z += driftv * (hiter->second->get_avg_t() - approximate_cluster_path_length / speed_of_light_cm_ns);
      }
      phibin = geo->get_phibin(phi);
      if (phibin < 0 || phibin >= nphibins)
      {
        continue;
      }
      double phidisp = phi - geo->get_phicenter(phibin);

      zbin = geo->get_zbin(hiter->second->get_avg_z());
      if (zbin < 0 || zbin >= nzbins)
      {
        continue;
      }
      double zdisp = z - geo->get_zcenter(zbin);

      double edep = hiter->second->get_edep();
      if (verbosity > 1)
      {
        fHMeanEDepPerCell->Fill(float(*layer), z, edep);
      }
      if (verbosity > 2000) cout << "Find or start a cell for this hit" << endl;

      if ((*layer) < (unsigned int) num_pixel_layers)
      {  // MAPS + ITT
        unsigned long long longphibins = nphibins;
        unsigned long long key = zbin * longphibins + phibin;
        std::map<unsigned long long, PHG4Cell *>::iterator it = cellptmap.find(key);
        PHG4Cell *cell;
        if (it != cellptmap.end())
        {
          cell = it->second;
        }
        else
        {
          PHG4CellDefs::keytype akey = PHG4CellDefs::SizeBinning::genkey(*layer, zbin, phibin);
          cell = new PHG4Cellv1(akey);
          cellptmap[key] = cell;
        }
        cell->add_edep(hiter->first, edep);
        cell->add_edep(edep);
        cell->add_shower_edep(hiter->second->get_shower_id(), edep);
        if (hiter->second->has_property(PHG4Hit::prop_eion)) cell->add_eion(hiter->second->get_eion());
      }
      else
      {  // TPC
        // converting Edep to Total Number Of Electrons
        float eion = hiter->second->get_eion();
        if (!isfinite(eion))
        {
          eion = edep;
        }
        if (eion <= 0)  // no ionization energy - skip to next hit
        {
          continue;
        }
        double nelec = gsl_ran_poisson(RandomGenerator, elec_per_gev * eion);
        if (verbosity > 1)
        {
          fHElectrons->Fill(nelec);
        }

        // The resolution due to pad readout is dominated by the charge spread during GEM multiplication, which we hard code because it is not a matter of opinion!
        double sigmaT = 0.04;  // 400 microns, charge dispersion at pad due to GEM stage, from Tom (see 8/11 email)
        // We use a double gaussian to represent the smearing due to the SAMPA chip shaping time - default values of fShapingLead and fShapingTail are 0.19 and 0.285 cm
        double sigmaL[2];
        // These are calculated (in cm) in the macro from the shaping RMS times and the gas drift velocity
        sigmaL[0] = fShapingLead;
        sigmaL[1] = fShapingTail;
        double cloud_sig_rp = sqrt(fDiffusionT * fDiffusionT * (fHalfLength - fabs(hiter->second->get_avg_z())) + sigmaT * sigmaT);
        double cloud_sig_zz[2];
        cloud_sig_zz[0] = sqrt(fDiffusionL * fDiffusionL * (fHalfLength - fabs(hiter->second->get_avg_z())) + sigmaL[0] * sigmaL[0]);
        cloud_sig_zz[1] = sqrt(fDiffusionL * fDiffusionL * (fHalfLength - fabs(hiter->second->get_avg_z())) + sigmaL[1] * sigmaL[1]);

        //===============
        // adding a random displacement, parameter is set in the macro
        // This just randomly offsets the cluster position to simulate the cluster resolution expected from a back of the enevelope calculation
        phi += fFractRPsm * gsl_ran_gaussian(RandomGenerator, cloud_sig_rp) / r;
        if (phi > +M_PI) phi -= 2 * M_PI;
        if (phi < -M_PI) phi += 2 * M_PI;
        z += fFractZZsm * gsl_ran_gaussian(RandomGenerator, cloud_sig_zz[0]);

        // moving center
        phibin = geo->get_phibin(phi);
        zbin = geo->get_zbin(z);
        // bin protection
        if (phibin < 0 || phibin >= nphibins)
        {
          continue;
        }
        if (zbin < 0 || zbin >= nzbins)
        {
          continue;
        }
        // bincenter correction
        phidisp = phi - geo->get_phicenter(phibin);
        zdisp = z - geo->get_zcenter(zbin);
        // increase cloud sigma too
        cloud_sig_rp *= (1 + fFractRPsm);
        cloud_sig_zz[0] *= (1 + fFractZZsm);
        cloud_sig_zz[1] *= (1 + fFractZZsm);

        if (verbosity > 2000)
          cout << "After adding random displacement: phi = " << phi << " z = " << z << " phibin = " << phibin << " zbin = " << zbin << endl
               << " phidisp = " << phidisp << " zdisp = " << zdisp << " new cloud_sig_rp " << cloud_sig_rp << " cloud_sig_zz[0] " << cloud_sig_zz[0]
               << " cloud_sig_zz[1] " << cloud_sig_zz[1] << endl;

        //=====

        // We should account for the fact that angled tracks deposit charge in a range of Z values, increasing the cluster Z length
        // The new software is proposed to deal with this by using a single volume with a step size of ~0.3 cm, so 4 steps per layer. Let's use 7.
        // Start with the entry and exit Z values for the track in this layer. These have to come from the G4 hit.
        // Make nseg  segment centers - keep nseg odd - process each segment through the shaper and ADC binning

        // Note that we ignore the difference in drift-diffusion between segments here, for convenience - it will be tiny

        double zrange = fabs(hiter->second->get_z(1) - hiter->second->get_z(0));
        if (verbosity > 2000) cout << " *********** zrange " << zrange << " zout " << hiter->second->get_z(1) << " zin " << hiter->second->get_z(0) << endl;

	int nseg = (int)zrange/cloud_sig_zz[0];  // must be odd number
	if(nseg%2==0)nseg+=1;
        // loop over the segment centers and distribute charge to the cells from each one
        for (int izr = 0; izr < nseg; izr++)
        {
          double zsegoffset = (izr - nseg / 2) * zrange / nseg;

          // offsetting z from the average for the layer may change the zbin, fix that
          int zbinseg = zbin = geo->get_zbin(z + zsegoffset);
          if (zbinseg < 0 || zbinseg >= nzbins)
          {
            continue;
          }
          double zdispseg = z + zsegoffset - geo->get_zcenter(zbinseg);

          if (verbosity > 2000) cout << " ---------- segment izr = " << izr << " with zsegoffset " << zsegoffset << " zbinseg " << zbinseg << " zdispseg " << zdispseg << endl;

          // Now:
          //    spread the charge in Z using the sigma due to the SAMPA chip shaping time
          //    spread the charge in r-phi using the sigma due to the drift-diffusion and GEM stack broadening

          int n_rp = int(3 * cloud_sig_rp / (r * phistepsize) + 1);
          int n_zz = int(3 * (cloud_sig_zz[0] + cloud_sig_zz[1]) / (2.0 * zstepsize) + 1);
          if (verbosity > 1)
          {
            fHErrorRPhi->Fill(float(*layer), z, cloud_sig_rp);
            fHErrorZ->Fill(float(*layer), z, cloud_sig_zz[0]);
            fHWindowP->Fill(float(*layer), z, n_rp);
            fHWindowZ->Fill(float(*layer), z, n_zz);
          }
          double cloud_sig_rp_inv = 1. / cloud_sig_rp;
          double cloud_sig_zz_inv[2];
          cloud_sig_zz_inv[0] = 1. / cloud_sig_zz[0];
          cloud_sig_zz_inv[1] = 1. / cloud_sig_zz[1];
          if (verbosity > 1000)
          {
            std::cout << " Summary: Z PHI " << z << " " << phi << " edep " << edep * 1e6 << " nelec " << nelec
                      << " cloud_sig_rp " << cloud_sig_rp << endl
                      << " TPC shaping RMS leading " << fShapingLead
                      << " TPC shaping RMS tail " << fShapingTail << endl
                      << " cloud_sig_zz[0] " << cloud_sig_zz[0] << " cloud_sig_zz[1] " << cloud_sig_zz[1] << std::endl;
            std::cout << " bin search window: nrp " << n_rp << " nzz " << n_zz << std::endl;
          }
          for (int iphi = -n_rp; iphi != n_rp + 1; ++iphi)
          {
            int cur_phi_bin = phibin + iphi;
            // correcting for continuity in phi
            if (cur_phi_bin < 0)
              cur_phi_bin += nphibins;
            else if (cur_phi_bin >= nphibins)
              cur_phi_bin -= nphibins;
            if ((cur_phi_bin < 0) || (cur_phi_bin >= nphibins))
            {
              std::cout << "PHG4CylinderCellTPCReco => error in phi continuity. Skipping" << std::endl;
              continue;
            }
            // Get the integral of the charge probability distribution in phi inside the current phi step
            double phiLim1 = 0.5 * M_SQRT2 * ((iphi + 0.5) * phistepsize * r - phidisp * r) * cloud_sig_rp_inv;
            double phiLim2 = 0.5 * M_SQRT2 * ((iphi - 0.5) * phistepsize * r - phidisp * r) * cloud_sig_rp_inv;
            double phi_integral = 0.5 * (erf(phiLim1) - erf(phiLim2));
            if (verbosity > 2000) cout << " current phi bin " << cur_phi_bin << " phiLim1 " << phiLim1 << " phiLim2 " << phiLim2 << " phi_integral " << phi_integral << endl;
            for (int iz = -n_zz; iz != n_zz + 1; ++iz)
            {
              int cur_z_bin = zbinseg + iz;
              if ((cur_z_bin < min_cell_zbin) || (cur_z_bin > max_cell_zbin)) continue;
              // Get the integral of the charge probability distribution in Z inside the current Z step. We only need to get the relative signs correct here, I think
              // this is correct for z further from the membrane - charge arrives early
              double zLim1 = 0.5 * M_SQRT2 * ((iz + 0.5) * zstepsize - zdispseg) * cloud_sig_zz_inv[0];
              double zLim2 = 0.5 * M_SQRT2 * ((iz - 0.5) * zstepsize - zdispseg) * cloud_sig_zz_inv[0];
              // The above is correct if we are in the leading part of the time distribution. In the tail of the distribution we use the second gaussian width
              // this is correct for z  closer to the membrane - charge arrives late
              if (zLim1 > 0)
                zLim1 = 0.5 * M_SQRT2 * ((iz + 0.5) * zstepsize - zdispseg) * cloud_sig_zz_inv[1];
              if (zLim2 > 0)
                zLim2 = 0.5 * M_SQRT2 * ((iz - 0.5) * zstepsize - zdispseg) * cloud_sig_zz_inv[1];
              // 1/2 * the erf is the integral probability from the argument Z value to zero, so this is the integral probability between the Z limits
              double z_integral = 0.5 * (erf(zLim1) - erf(zLim2));
              float neffelectrons = (2000 / nseg) * nelec * (phi_integral * z_integral);  // adding constant electron avalanche (value chosen so that digitizer will not trip)

              if (verbosity > 2000)
                cout << "    cur_z_bin " << cur_z_bin << "  center z " << geo->get_zcenter(cur_z_bin) << " center r-phi " << geo->get_radius() * geo->get_phicenter(cur_phi_bin) << endl
                     << "            zLim1 " << zLim1 << " zLim2 " << zLim2 << " z_integral " << z_integral << " neffelectrons " << neffelectrons << endl;

              if (verbosity > 5000)
              {
                std::cout << Form("%.3f", neffelectrons) << " ";
                if (iz == n_zz) std::cout << std::endl;
              }
              if (neffelectrons < 0) continue;  // skip no signals
              unsigned long key = cur_z_bin * nphibins + cur_phi_bin;
              std::map<unsigned long long, PHG4Cell *>::iterator it = cellptmap.find(key);
              PHG4Cell *cell;
              if (it != cellptmap.end())
              {
                cell = it->second;
              }
              else
              {
                PHG4CellDefs::keytype akey = PHG4CellDefs::SizeBinning::genkey(*layer, cur_z_bin, cur_phi_bin);
                cell = new PHG4Cellv1(akey);
                cellptmap[key] = cell;
              }
              if (verbosity > 2000) cout << "    adding edep = neffelectrons = " << neffelectrons << " to cell with key = " << key << endl;
              cell->add_edep(hiter->first, neffelectrons);
              cell->add_edep(neffelectrons);
              cell->add_shower_edep(hiter->second->get_shower_id(), neffelectrons);
              if (hiter->second->has_property(PHG4Hit::prop_eion)) cell->add_eion(hiter->second->get_eion());
            }  //iz
          }    //iphi
        }      // izr
      }
    }
    int count = 0;
    for (std::map<unsigned long long, PHG4Cell *>::iterator it = cellptmap.begin(); it != cellptmap.end(); ++it)
    {
      cells->AddCell(it->second);
      int phibin = PHG4CellDefs::SizeBinning::get_phibin(it->second->get_cellid());
      int zbin = PHG4CellDefs::SizeBinning::get_zbin(it->second->get_cellid());
      if (verbosity > 1)
      {
        float zthis = geo->get_zcenter(zbin);
        fHMeanElectronsPerCell->Fill(float(*layer), zthis, it->second->get_edep());
      }
      if (verbosity > 2000)
        std::cout << " Adding phibin " << phibin << " zbin " << zbin << " with edep " << it->second->get_edep() << std::endl;
      count += 1;
    }
    if (verbosity > 1000)
      std::cout << " || Number of cells hit " << count << std::endl;
  }
  if (verbosity > 1000) std::cout << "PHG4CylinderCellTPCReco end" << std::endl;
  _timer.get()->stop();
  return Fun4AllReturnCodes::EVENT_OK;
}
