#include "SineSubtract.h" 
#include "TGraph.h" 
#include "TCanvas.h" 
#include "TStyle.h" 

#ifdef SINE_SUBTRACT_PROFILE
#include "TStopwatch.h"
#endif

#include "TMath.h"
#include <set>
#include "FFTtools.h"
#include "TF1.h" 
#include "TH2.h"


#ifdef ENABLE_VECTORIZE
#include "vectormath_trig.h" 

//assume AVX2 is present, otherwise it'll be simulated 

#ifdef SINE_SUBTRACT_USE_FLOATS
#define VEC Vec8f 
#define VEC_N 8
#define VEC_T float
#else
#define VEC Vec4d 
#define VEC_N 4 
#define VEC_T double 
#endif


#endif 

extern int gErrorIgnoreLevel; // who ordered that? 




static double normalize_angle(double phi)
{
  return  phi - 2*TMath::Pi() * floor((phi+ TMath::Pi()) / (2.*TMath::Pi())); 
}

FFTtools::SineFitter::SineFitter()
{
  min.SetFunction(f); 
  verbose = false; 

}

FFTtools::SineFitter::~SineFitter()
{


}

void FFTtools::SineFitter::setGuess(double fg, int ntrace, const double * phg, const  double * ampg)
{
  freq = fg; 
  phase.clear(); 
  amp.clear(); 
  phase_err.clear(); 
  amp_err.clear(); 

  phase.insert(phase.end(), phg, phg + ntrace); 
  amp.insert(amp.end(), ampg, ampg + ntrace); 
  phase_err.insert(phase_err.end(), 3, 0); 
  amp_err.insert(amp_err.end(), 3, 0); 
}

ROOT::Math::IBaseFunctionMultiDim* FFTtools::SineFitter::SineFitFn::Clone() const
{
  SineFitFn * fn = new SineFitFn; 
#ifdef SINE_SUBTRACT_USE_FLOATS
  fn->setXY(nt,ns,xp,yp); //incredibly inefficient, but quick kludge for now 
#else
  fn->setXY(nt,ns,x,y); 
#endif
  return fn; 
}

double FFTtools::SineFitter::SineFitFn::DoDerivative(const double * p, unsigned int coord) const
{

  double w = 2 *TMath::Pi() * p[0]; 
  double deriv = 0; 
  int type = coord == 0 ? 0 : (1 + ((coord-1) % 2)); 

  for (int ti = 0; ti < nt; ti++) 
  {

    if (type > 0 && (int(coord) - 1)/2 != ti) continue; 
    double ph = normalize_angle(p[1 + 2*ti]); 
    double A = p[2+2*ti]; 

#ifdef ENABLE_VECTORIZE 

    VEC vecx; 
    VEC vecy; 
    VEC vec_ph(ph); 
    VEC vec_w(w); 
    VEC vec_A(A); 
    int leftover = ns % VEC_N;
    int nit = ns/VEC_N + (leftover ? 1 : 0); 

    for (int i = 0; i < nit; i++)
    {
      vecx.load(x[ti]+VEC_N*i); 
      vecy.load(y[ti]+VEC_N*i); 
      VEC vec_ang = mul_add(vecx, vec_w, vec_ph); 
      VEC vec_cos; 
      VEC vec_sin = sincos(&vec_cos, vec_ang); 
      VEC vec_Y = mul_sub(vec_sin, vec_A, vecy); 

      VEC dYdp; 

      switch (type) 
      {
        case 0: 
          dYdp = vec_A *vecx*vec_cos * (2 * M_PI); 
          break; 
        case 1: 
          dYdp = vec_A *vec_cos; 
          break; 
        case 2: 
          dYdp = vec_sin; 
          break; 
      }
#ifndef SINE_SUBTRACT_DONT_HORIZONTAL_ADD
      if (i == ns-1 && leftover) //hopefully this gets unrolled? 
      {
        vec_Y.cutoff(VEC_N-leftover); 
      }

      deriv +=horizontal_add( vec_Y * dYdp)/ns; 
#else

      VEC ans = vec_Y * dYdp; 
      VEC_T ans_v[VEC_N]; 
      ans.store(ans_v); 

      int vecn = (i == nit-1 && leftover) ? leftover : VEC_N;
      for (int j = 0; j < vecn; j++) 
      {
        deriv += ans_v[j]/ns; 
      }

#endif
    }

#else
    for (int i = 0; i < ns; i++)
    {
      double t = x[ti][i]; 
      double sinang = sin(w*t + ph); 
      double Y = A *sinang; 
      double dYdp = 0; 

      switch(type) 
      {
        case 0: 
          dYdp = A*t*cos(w*t + ph) * 2 * TMath::Pi(); 
          break; 
        case 1: 
          dYdp = A*cos(w*t + ph); 
          break; 
        case 2:
          dYdp = sinang; 
          break; 
      }

      deriv += ((Y-y[ti][i]) * dYdp)/ns; 
    }
#endif
  }

  deriv *=2; 

//  printf("dP/d%s (f=%f, ph=%f, A=%f) = %f\n", coord == 0 ? "f" : coord == 1? "ph" : "A", p[0], ph, A, deriv);  
  return deriv/nt; 
}



double FFTtools::SineFitter::SineFitFn::DoEval(const double * p) const
{

  double w = 2 *TMath::Pi() * p[0]; 

  double power = 0; 


  for (int ti = 0; ti < nt; ti++)
  {
    double ph = normalize_angle(p[1+2*ti]); 
    double A = p[2+2*ti]; 

#ifdef ENABLE_VECTORIZE 
    VEC vecx; 
    VEC vecy; 
    VEC vec_ph(ph); 
    VEC vec_w(w); 
    VEC vec_A(A); 

    int leftover = ns % VEC_N;
    int nit = ns/VEC_N + (leftover ? 1 : 0); 

    for (int i = 0; i < nit; i++)
    {
      vecx.load(x[ti]+VEC_N*i); 
      vecy.load(y[ti]+VEC_N*i); 
      VEC vec_ang = mul_add(vecx, vec_w, vec_ph); 
      VEC vec_sin = sin(vec_ang); 
      VEC vec_Y = mul_sub(vec_sin, vec_A, vecy); 

      VEC vec_Y2 = square(vec_Y); 
#ifndef SINE_SUBTRACT_DONT_HORIZONTAL_ADD
      if (i == nit-1 && leftover) //hopefully this gets unrolled? 
      {
        vec_Y2.cutoff(VEC_N-leftover); 
      }


      power += horizontal_add(vec_Y2)/ns; 
#else 
      int vecn = (i == nit-1 && leftover) ? leftover : VEC_N;
      for (int j = 0; j< vecn; j++)
      {
        power += vec_Y2[j]/ns; 
      }

#endif
    }
#else
    for (int i = 0; i < ns; i++)
    {
      double Y = A * sin(w*x[ti][i] + ph) -y[ti][i]; 
      power += Y*Y/ns; 
    }
#endif 

  }
//  printf("P(f=%f, ph=%f, A=%f) = %f\n", p[0], ph, A, power);  
  return power/nt;

}

void FFTtools::SineFitter::SineFitFn::setXY(int ntraces, int nsamples, const double ** xx, const double ** yy) 
{
#ifdef SINE_SUBTRACT_USE_FLOATS
  // we have to convert everything to floats... 
  if (nt!=ntraces)
  {

    float ** newx = (float**) malloc(ntraces * sizeof(float*)); 
    float ** newy = (float**) malloc(ntraces * sizeof(float*)); 
    for (int i = 0; i < nt; i++) 
    {
      if (i < ntraces)
      {
        newx[i] = x[i]; 
        newy[i] = y[i]; 
      }
      else
      {
        free(x[i]); 
        free(y[i]); 
      }
    }

    for (int i = nt; i < ntraces; i++) 
    {
      newx[i] = 0; 
      newy[i] = 0; 
    }


    free(x); 
    free(y); 
    x = newx; 
    y = newy; 
  }

  if (ns!=nsamples)
  {
    for (int i = 0; i < ntraces; i++) 
    {
      x[i] = (float*) realloc(x[i], nsamples * sizeof(float)); 
      y[i] = (float*) realloc(y[i], nsamples * sizeof(float)); 
    }
  }


  for (int j = 0; j < ntraces; j++) 
  {
    for (int i = 0; i < nsamples; i++) 
    {
       x[j][i] = xx[j][i]; 
       y[j][i] = yy[j][i]; 
    }
  }

  xp = xx; 
  yp = yy; 
  
#else
  x = xx; 
  y = yy;
#endif
  nt = ntraces; 
  ns = nsamples;
}

static char * arrString(int n, const  double * arr, int sigfigs =8 ) 
{
  int size = 2 + (n-1) + (sigfigs+5) * n + 2;
  
  char * ret = new char[size]; 
  char format[8]; 
  sprintf(format,"%%.%dg",sigfigs); 

  int ctr = 0; 
  ctr += sprintf(ret + ctr, "("); 
  for (int i = 0; i < n; i++) 
  {
    ctr+=sprintf(ret + ctr, format, arr[i]); 
    if (i < n-1)  ctr += sprintf(ret + ctr, ","); 
  }
  ctr+= sprintf(ret+ctr, ")"); 

  assert(ctr < size); 

  return ret; 
}



void FFTtools::SineFitter::doFit(int ntraces, int nsamples, const double ** x, const double **y)
{
  f.setXY(ntraces,nsamples,x,y); 

  if(verbose) 

  if (verbose) 
  {
    char * Astr = arrString(ntraces,&amp[0]); 
    char * Phstr =arrString(ntraces,&phase[0]); 
    printf("Guesses: f= %f, A=%s, ph=%s", freq, Astr, Phstr); 

    double p[1 + 2*ntraces]; 
    p[0] = freq; 
    for (int i =0; i < ntraces; i++) 
    {
      p[1+2*i] = phase[i]; 
      p[2+2*i] = amp[i]; 
    }
    printf("Guess power is %f. Gradient is (", f.DoEval(p));
    for (int i = 0; i < 2 * ntraces+1; i++) 
    {
      printf("%f", f.DoDerivative(p,i)); 
      if (i < 2*ntraces) printf(","); 
    }
    printf(")\n"); 

  }

  double dt = (x[0][nsamples-1]  - x[0][0]) / (nsamples-1); 
  double fnyq = 1. / (2 * dt); 
  double df = fnyq / nsamples; 

  min.SetFunction(f); 
  min.SetLimitedVariable(0, "f",freq, df/10., freq-df, freq+df); 


  for (int i = 0; i < ntraces; i++) 
  {
    min.SetVariable(1+2*i, TString::Format("phi%d",i).Data(),phase[i], TMath::Pi()/nsamples); 
    min.SetLimitedVariable(2+2*i, TString::Format("A%d",i).Data(), amp[i], 1./sqrt(amp[i]), 0.25 * amp[i], 4 *amp[i]); 
  }


  min.SetPrintLevel(verbose ? 1 : 0); 

  int old_level = gErrorIgnoreLevel; 
  if (!verbose)
  {
    gErrorIgnoreLevel = 1001; 
  }
  min.Minimize(); 
  gErrorIgnoreLevel = old_level; 
  if(verbose)  min.PrintResults(); 

  freq = min.X()[0]; 
  freq_err = min.Errors()[0]; 
  for (int i = 0; i < ntraces; i++) 
  {
    phase[i] = normalize_angle(min.X()[1+2*i]); 
    amp[i] = min.X()[2+2*i]; 
    phase_err[i] = min.Errors()[1+2*i]; 
    amp_err[i] = min.Errors()[2+2*i]; 
  }
}


FFTtools::SineSubtract::SineSubtract(int maxiter, double min_power_reduction, const FFTWindowType * win, bool store)
  : maxiter(maxiter), min_power_reduction(min_power_reduction), store(store), w(win) 
{

  neighbor_factor2 = 0.15; 
  verbose = false; 
  tmin = 0; 
  tmax = 0; 

  fmin = 0; 
  fmax = 0; 
  
}

void FFTtools::SineSubtractResult::append(const SineSubtractResult *r) 
{
  powers.insert(powers.end(), r->powers.begin(), r->powers.end()); 
  freqs.insert(freqs.end(), r->freqs.begin(), r->freqs.end()); 
  freqs_errs.insert(freqs_errs.end(), r->freqs_errs.begin(), r->freqs_errs.end()); 

  for (size_t i = 0; i < phases.size(); i++) 
  {
    phases[i].insert(phases[i].end(), r->phases[i].begin(), r->phases[i].end()); 
    amps[i].insert(amps[i].end(), r->amps[i].begin(), r->amps[i].end()); 
    phases_errs[i].insert(phases_errs[i].end(), r->phases_errs[i].begin(), r->phases_errs[i].end()); 
    amps_errs[i].insert(amps_errs[i].end(), r->amps_errs[i].begin(), r->amps_errs[i].end()); 
  }
}


void FFTtools::SineSubtractResult::clear() 
{
  powers.clear(); 
  freqs.clear(); 
  phases.clear(); 
  amps.clear(); 
  freqs_errs.clear(); 
  phases_errs.clear(); 
  amps_errs.clear(); 
}


void FFTtools::SineSubtract::reset() 
{

  r.clear(); 
  for (unsigned i = 0; i < gs.size(); i++)
  {
    for (unsigned j = 0; j < gs[i].size(); j++) 
    {
      delete gs[i][j]; 
    }
  }
  gs.clear(); 
  for (unsigned i = 0; i < spectra.size(); i++) delete spectra[i]; 
  for (unsigned i = 0; i < fft_phases.size(); i++)
  {
    for (unsigned j = 0; j < fft_phases[i].size(); j++) 
    {
      delete fft_phases[i][j]; 
    }
  }
  spectra.clear(); 
  fft_phases.clear(); 


}

TGraph * FFTtools::SineSubtract::subtractCW(const TGraph * g, double dt) 
{
  TGraph * gcopy = new TGraph(g->GetN(), g->GetX(), g->GetY()); 
  subtractCW(1,&gcopy,dt); 
  return gcopy; 
}

void FFTtools::SineSubtract::subtractCW(int ntraces, TGraph ** g, double dt) 
{


#ifdef SINE_SUBTRACT_PROFILE
  TStopwatch sw; 
#endif
  reset(); 

  std::multiset<int> failed_bins; //nfails for each bin 


  int low = tmin < 0 || tmin >= g[0]->GetN() ? 0 : tmin; 
  int high = tmax <= 0 || tmax > g[0]->GetN() ? g[0]->GetN() : tmax; 



  int Nuse = high - low; 

  //zero mean



  double power = 0; 
  for (int ti = 0; ti < ntraces; ti++) 
  {
    double mean = g[ti]->GetMean(2); 
    for (int i = low; i <high ; i++)
    {
      g[ti]->GetY()[i] -=mean; 
      power += g[ti]->GetY()[i] * g[ti]->GetY()[i]; 
    }
  }


  r.powers.push_back(power/Nuse/ntraces); 

  if (store) 
  {
    gs.insert(gs.end(), ntraces, std::vector<TGraph*>()); 
    fft_phases.insert(fft_phases.end(), ntraces, std::vector<TGraph*>()); 

    for (int i = 0; i < ntraces; i++) 
    {
      g[i]->SetTitle(TString::Format("Initial Waveform %d",i)); 
      gs[i].push_back(new TGraph(*g[i])); 
    }
  }

  int ntries = 0; 
  FFTWComplex * ffts[ntraces]; 

  int nzeropad = 1 << (32 - __builtin_clz(Nuse-1)); 
//  printf("%d\n",nzeropad); 
  int fftlen = nzeropad/2+1; 

  r.phases.insert(r.phases.end(),ntraces, std::vector<double>()); 
  r.phases_errs.insert(r.phases_errs.end(),ntraces, std::vector<double>()); 
  r.amps.insert(r.amps.end(),ntraces, std::vector<double>()); 
  r.amps_errs.insert(r.amps_errs.end(),ntraces, std::vector<double>()); 

  int nattempts = 0; 
  double realdt = dt  > 0 ? dt : g[0]->GetX()[1] - g[0]->GetX()[0]; 
  while(true) 
  {

    nattempts++; 
    for (int ti = 0; ti  < ntraces; ti++)
    {
      TGraph * gcropped = new TGraph(Nuse, g[ti]->GetX() + low, g[ti]->GetY() + low); 
      TGraph * ig = dt <= 0? gcropped : FFTtools::getInterpolatedGraph(gcropped, dt); 
      applyWindow(ig,w); 
      ig->Set(nzeropad); 
      ffts[ti] = FFTtools::doFFT(nzeropad, ig->GetY()); 
      if (dt >0) delete ig; 
      delete gcropped; 
    }


    int max_i = -1; 
    double mag2[fftlen]; 
    memset(mag2, 0, sizeof(mag2)); 
    double max_adj_mag2 = 0; 

    double max_f = 0; 


    double * spectra_x=0, *spectra_y=0; 
    double * phase[ntraces]; 

    if (store)
    {
      spectra_x  = new double[fftlen]; 
      spectra_y  = new double[fftlen]; 
      for (int ti = 0; ti < ntraces; ti++) 
      {
        phase[ti]  = new double[fftlen]; 
      }
    }

    //find lone peak magnitude of FFT 
    
    for (int ti = 0; ti < ntraces; ti++) 
    {
      for (int i = 0; i < fftlen; i++)
      {
        mag2[i] += ffts[ti][i].getAbsSq(); 
        if (store) 
        {
          phase[ti][i] = ffts[ti][i].getPhase(); 
        }
      }
    }


    double df = 0.5 / (realdt * nzeropad); 
    for (int i = 0; i < fftlen; i++)
    {
      double freq =  i / (realdt * nzeropad); 

      double adj_mag2 = mag2[i] / (1+failed_bins.count(i)); 
      double neigh_mag2_low = i == 0 ? DBL_MAX : mag2[i-1];; 
      double neigh_mag2_high = i == fftlen-1 ? DBL_MAX : mag2[i+1]; 


      if (store) 
      {
        spectra_x[i] = i /(realdt * nzeropad); 
        spectra_y[i] = mag2[i]/nzeropad/ntraces; 
        if (i > 0 && i < fftlen-1) spectra_y[i]*=2; 
//        printf("%f %f\n", spectra_x[i], spectra_y[i]); 

      }

      if ( (fmin > 0) && (freq + df < fmin))
      {
        continue; 
      }
        

      if ( (fmax  > 0) && (freq - df > fmax)) 
      {
        continue; 
      }


      if ( max_i < 0 ||
         ( adj_mag2 > max_adj_mag2 && 
           mag2[i] * neighbor_factor2 > TMath::Min(neigh_mag2_low, neigh_mag2_high)
           )
         )
      {

        
        max_i = i; 
        max_adj_mag2 =adj_mag2; 
        max_f = freq;
      }
    }


    double max_ph[ntraces]; 
    double max_A[ntraces]; 

    for (int ti = 0; ti < ntraces; ti++)
    {
      max_ph[ti] = ffts[ti][max_i].getPhase(); 
      max_A[ti] = 2*ffts[ti][max_i].getAbs()/nzeropad; 
      delete [] ffts[ti]; 
    }



    fitter.setGuess(max_f, ntraces, max_ph, max_A); 

    const double * x[ntraces];
    const double * y[ntraces];

    for (int i = 0; i < ntraces; i++) 
    {
      x[i] = g[i]->GetX()+low; 
      y[i] = g[i]->GetY()+low; 
    }

    fitter.doFit(ntraces, Nuse, x,y); 

    power = fitter.getPower(); 

    double ratio = 1. - power /r.powers[r.powers.size()-1]; 
    if (verbose) printf("Power Ratio: %f\n", ratio); 

    if(store && (ratio >= min_power_reduction || ntries == maxiter))
    {
      spectra.push_back(new TGraph(fftlen,spectra_x, spectra_y)); 
      if (r.powers.size() > 1)
      {
        spectra[spectra.size()-1]->SetTitle(TString::Format("Spectrum after %lu iterations",r.powers.size()-1)); 
      }
      else
      {
        spectra[spectra.size()-1]->SetTitle("Initial spectrum"); 
      }
      for (int ti = 0; ti < ntraces; ti++) 
      {
        fft_phases[ti].push_back(new TGraph(fftlen, spectra_x, phase[ti])); 
        if (r.powers.size() > 1)
        {
          fft_phases[ti][fft_phases[ti].size()-1]->SetTitle(TString::Format("Wf %d Phase after %lu iterations",ti,r.powers.size()-1)); 
        }
        else
        {
          fft_phases[ti][fft_phases[ti].size()-1]->SetTitle(TString::Format("Wf %d Initial Phase", ti)); 
        }
  
        delete [] phase[ti]; 

      }
      delete [] spectra_x; 
      delete [] spectra_y; 
    }



    if (ratio < min_power_reduction)
    {
      failed_bins.insert(max_i); 
      if (ntries++ > maxiter)   
      {
        break;
      }
      continue; 
    }

    r.powers.push_back(power); 
    r.freqs.push_back(fitter.getFreq()); 
    r.freqs_errs.push_back(fitter.getFreqErr()); 


    for (int i = 0; i < ntraces; i++) 
    {
      r.phases[i].push_back(fitter.getPhase()[i]); 
      r.phases_errs[i].push_back(fitter.getPhaseErr()[i]); 
      r.amps[i].push_back(fitter.getAmp()[i]); 
      r.amps_errs[i].push_back( fitter.getAmpErr()[i]); 
    }

    for (int ti = 0; ti < ntraces; ti++) 
    {
      for (int i = 0; i < g[ti]->GetN(); i++) 
      {
        g[ti]->GetY()[i] -= fitter.getAmp()[ti] * sin(2*TMath::Pi() * fitter.getFreq() *g[ti]->GetX()[i] + fitter.getPhase()[ti]); 
      }

      if (store) 
      {
        //add sine we subtracted to previous graph 
        TF1 * fn = new TF1(TString::Format("fitsin%lu_%d",r.powers.size(),ti), "[0] * sin(2*pi*[1] * x + [2])",g[ti]->GetX()[0], g[ti]->GetX()[g[ti]->GetN()-1]); 
        fn->SetParameter(0,fitter.getAmp()[ti]); 
        fn->SetParameter(1,fitter.getFreq()); 
        fn->SetParameter(2,fitter.getPhase()[ti]); 
        fn->SetNpx(2*g[ti]->GetN()); 
        gs[ti][gs[ti].size()-1]->GetListOfFunctions()->Add(fn); 

        //add new graph 
        g[ti]->SetTitle(TString::Format("Wf %d after %lu iterations",ti, r.powers.size()-1)); 
        gs[ti].push_back(new TGraph(*g[ti])); 
      }

    }
  }

#ifdef SINE_SUBTRACT_PROFILE
  printf("Time for SineSubtract::subtractCW(): "); 
  sw.Print(); 
  printf("nattempts: %d\n",nattempts); 

#endif
}


int FFTtools::SineSubtract::getNSines() const 
{
  return r.phases[0].size(); 
}

void FFTtools::SineSubtract::makeSlides(const char * title, const char * fileprefix, const char * outdir , const char * format) const 
{

  gROOT->SetBatch(kTRUE);
  gROOT->ForceStyle(kTRUE);

  Int_t orig_msz = gStyle->GetMarkerSize();
  Int_t orig_mst = gStyle->GetMarkerStyle();
  Int_t orig_lt  = gStyle->GetLineWidth();

  gStyle->SetMarkerSize(2);
  gStyle->SetMarkerStyle(20);
  gStyle->SetLineWidth(orig_lt*4);

  int orig_stat = gStyle->GetOptStat(); 
  gStyle->SetOptStat(0); 

 
  TCanvas canvas("slidecanvas","SlideCanvas",4000,3000); 
  canvas.Divide(2,2); 

  TGraph g; 
  int niter = spectra.size(); 
  TH2I poweraxis("poweraxis","Iteration 0", 10, 0,niter, 10,0, r.powers[0]*1.1); 
  poweraxis.GetXaxis()->SetTitle("iteration"); 
  poweraxis.GetYaxis()->SetTitle("power"); 
  if (gs.size() > 1) 
  {
    canvas.cd(2)->Divide(1,gs.size()); 
    canvas.cd(4)->Divide(1,gs.size()); 
  }

  FILE * texfile = fopen(TString::Format("%s/%s.tex", outdir, fileprefix),"w"); 

  for (int i = 0; i < niter; i++) 
  {

    canvas.cd(1); 
    poweraxis.SetTitle(i < niter -1 ? TString::Format("Iteration %d, Sub Freq = %g Ghz",i, r.freqs[i]) : TString::Format("Iteration %d (final)", i)); 

    poweraxis.Draw(); 
    g.SetPoint(i, i, r.powers[i]); 
    g.Draw("lp"); 
    canvas.cd(2); 

    int old_gs_width[gs.size()];

    for (size_t j = 0; j < gs.size(); j++) 
    {
      if (gs.size() > 1) 
      {
        canvas.cd(2)->cd(j+1); 
      }
      old_gs_width[j]= gs[j][i]->GetLineWidth(); 
      gs[j][i]->SetLineWidth(3); 
      gs[j][i]->Draw("al"); 
    }


    canvas.cd(3)->SetLogy(); 

    int old_spectrum_width = spectra[i]->GetLineWidth(); 
    spectra[i]->SetLineWidth(5); 
    spectra[i]->Draw("alp"); 

    canvas.cd(4); 
    int old_phases_width[gs.size()];
    for (size_t j = 0; j < gs.size(); j++) 
    {
      if (gs.size() > 1) 
      {
        canvas.cd(4)->cd(j+1); 
      }
      old_phases_width[j] = fft_phases[j][i]->GetLineWidth(); 
      fft_phases[j][i]->SetLineWidth(3); 
      fft_phases[j][i]->Draw("alp"); 
    }

    TString canvfile = TString::Format("%s/%s_%d.%s", outdir, fileprefix, i, format); 
    canvas.SaveAs(canvfile); 

    spectra[i]->SetLineWidth(old_spectrum_width); 
    for (size_t j = 0; j < gs.size(); j++) 
    {
      gs[j][i]->SetLineWidth(old_gs_width[j]); 
      fft_phases[j][i]->SetLineWidth(old_phases_width[j]); 
    }

    fprintf(texfile, "\\begin{frame}\n"); 
    fprintf(texfile, "\t\\frametitle{%s (iteration %d)}\n", title, i); 
    fprintf(texfile, "\t\\begin{center}\n"); 
    fprintf(texfile, "\t\t\\includegraphics[width=4.2in]{%s_%d}\n",fileprefix,i); 
    fprintf(texfile, "\t\\end{center}\n"); 
    fprintf(texfile, "\\end{frame}\n\n"); 
    fflush(texfile); 
  }

  fclose(texfile); 

  gROOT->ForceStyle(kFALSE);
  gROOT->SetBatch(kFALSE);

  gStyle->SetMarkerSize(orig_msz);
  gStyle->SetMarkerStyle(orig_mst);
  gStyle->SetLineWidth(orig_lt);
  gStyle->SetOptStat(orig_stat); 
}

void FFTtools::SineSubtract::makePlots(TCanvas * cpower, TCanvas * cw, int ncols) const 
{

  int nplots = spectra.size() * (1 + gs.size()); 
  if (!cpower) cpower = new TCanvas("sinesubtractpowerplot","SineSubtract Power Evolution",1000,600); 
  cpower->Clear(); 

  if (!cw) cw = new TCanvas("sinesubtractplots","SineSubtract Waveforms/Spectra",1000,600); 
  cw->Clear(); 
  cw->Divide(ncols, (nplots-1)/ncols +1); 


  std::vector<double> iterations(r.powers.size()); 
  for (unsigned i = 0; i < r.powers.size(); i++) iterations[i] = i;
  TGraph *gpower = new TGraph(r.powers.size(), &iterations[0], &r.powers[0]); 
  cpower->cd(); 
  gpower->SetTitle("Power vs. iteration"); 
  gpower->Draw("alp"); 


  for (size_t i = 0; i < spectra.size(); i++) 
  {
    cw->cd((1+gs.size())*i+1)->SetLogy(); 
    ((TGraph*)spectra[i])->Draw("alp"); 
    for (size_t j = 0; j < gs.size(); j++)
    {
      cw->cd((1+gs.size())*i+2+j); 
      ((TGraph*)gs[j][i])->Draw("alp"); 
    }
  }

}


FFTtools::SineSubtract::~SineSubtract()
{
  reset(); 
}


ClassImp(FFTtools::SineSubtractResult); 

