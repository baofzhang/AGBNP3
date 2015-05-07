/* -------------------------------------------------------------------------- *
 *                                   AGBNP3                                   *
 * -------------------------------------------------------------------------- *
 * This file is part of the AGBNP3 implicit solvent model implementation      *
 * funded by the National Science Foundation under grant:                     *
 * NSF SI2 1440665  "SI2-SSE: High-Performance Software for Large-Scale       *
 * Modeling of Binding Equilibria"                                            *
 *                                                                            *
 * copyright (c) 2015 by the Authors.                                         *
 * Authors: Emilio Gallicchio <emilio.gallicchio@gmail.com>                   *
 * Contributors:                                                              *
 *                                                                            *
 * The software may be used under the terms of the GNU Lesser General Public  *
 * License (LGPL).  A copy of this license may be found at this address:      *
 * https://www.gnu.org/licenses/lgpl.html                                     *
 *                                                                            *
 * Permission is hereby granted, free of charge, to any person obtaining a    *
 * copy of this software and associated documentation files (the "Software"), *
 * to deal in the Software without restriction, including without limitation  *
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,   *
 * and/or sell copies of the Software, and to permit persons to whom the      *
 * Software is furnished to do so, subject to the following conditions:       *
 *                                                                            *
 * The above copyright notice and this permission notice shall be included in *
 * all copies or substantial portions of the Software.                        *
 *                                                                            *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    *
 * THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,    *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR      *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE  *
 * USE OR OTHER DEALINGS IN THE SOFTWARE.                                     *
 * -------------------------------------------------------------------------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <float.h>

#include "agbnp3_private.h"

#ifdef _OPENMP
#include <omp.h>
#endif

void agbnp3_errprint(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
#pragma omp critical
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

void agbnp3_free(void *x){
  if(x) free(x);
}


 /* Memory allocator. Aligns if using SIMD parallelization.*/
int agbnp3_memalloc(void **memptr, const size_t size){
#ifdef INTEL_MIC
  size_t alignment = 64;
#else
  size_t alignment = 16;
#endif
  int retcode = 0;
  void *mem;
  size_t padded_size;
  //make size a multiple of alignment
  padded_size = (size/alignment + 1)*alignment;
  retcode =  posix_memalign(&mem, alignment, padded_size);
  if(retcode) mem = NULL;
  *memptr = mem;
  return retcode;
}

/* calloc() equivalent */
int agbnp3_calloc(void **memptr, const size_t size){
  int retcode;
  void *mem;
  retcode = agbnp3_memalloc(&mem, size);
  if(retcode==0 && mem){
    memset(mem,0,size);
  }
  *memptr = mem;
  return retcode;
}

/* realloc equivalent */
int agbnp3_realloc(void **memptr, const size_t old_size, const size_t new_size){
  int retcode, size;
  void *mem;
  retcode = agbnp3_memalloc(&mem, new_size);
  if(*memptr && retcode==0 && mem){
    size = ( new_size > old_size ) ? old_size : new_size;
    memcpy(mem,*memptr,size);
    agbnp3_free(*memptr);
  }
  *memptr = mem;
  return retcode;
}

/* cubic spline setup */
/*
This code is based on the cubic spline interpolation code presented in:
Numerical Recipes in C: The Art of Scientific Computing
by
William H. Press,
Brian P. Flannery,
Saul A. Teukolsky, and
William T. Vetterling .
Copyright 1988 (and 1992 for the 2nd edition)

I am assuming zero-offset arrays instead of the unit-offset arrays
suggested by the authors.  You may style me rebel or conformist
depending on your point of view.

Norman Kuring	31-Mar-1999

*/
void agbnp3_cspline_setup(float dx, int n, float* y, 
			 float yp1, float ypn, 
			 float* y2){
  float* u;
  int	i,k;
  float	p,qn,sig,un;

  u = malloc(n*sizeof(float));
    
  if(yp1 > 0.99e30)
    y2[0] = u[0] = 0.0;
  else{
    y2[0] = -0.5;
    u[0] = (3.0/(dx))*((y[1]-y[0])/(dx)-yp1);
  }
  for(i = 1; i < n-1; i++){
    sig = 0.5;
    p = sig*y2[i-1] + 2.0;
    y2[i] = (sig - 1.0)/p;
    u[i] = (y[i+1] - y[i])/(dx) - (y[i] - y[i-1])/(dx);
    u[i] = (6.0*u[i]/(2.*dx) - sig*u[i-1])/p;
  }
  if(ypn > 0.99e30)
    qn = un = 0.0;
  else{
    qn = 0.5;
    un = (3.0/(dx))*(ypn - (y[n-1] - y[n-2])/(2.*dx));
  }
  y2[n-1] = (un - qn*u[n-2])/(qn*y2[n-2] + 1.0);
  for(k = n-2; k >= 0; k--){
    y2[k] = y2[k]*y2[k+1] + u[k];
  }

  agbnp3_free(u);
}

void agbnp3_cspline_interpolate(float x, float dx, int n, float* y, float* y2,
				float *f, float *fp){
  int k;
  float xh, b, a, a2, b2, kf, dp1, dp2, dxinv = 1./dx;
  
  dp1 = dx/6.0;
  dp2 = dx*dp1;

  xh = x*dxinv;
  k = xh;
  kf=k;
  if(k > n-2){
    *f = 0.0;
    *fp = 0.0;
    return;
  }
  a = (kf+1.0)-xh;
  a2 = a*a;
  b = xh - kf;
  b2 = b*b;
  *f = a*y[k]+b*y[k+1] +  ((a2*a - a)*y2[k] + (b2*b - b)*y2[k+1])*dp2;
  *fp = (y[k+1] - y[k])*dxinv - 
    ((3.*a2 - 1.)*y2[k] + (3.*b2 - 1.)*y2[k+1])*dp1;
}

/* prepares input for agbnp3_cspline_interpolate_soa() given  


/* applies cspline interpolation to a series of data:
k[]: table look up index
xh[]: x/dx
dx: spacing
m: number of points
yp[], y[]: node ordinates at k+1 and k
y2p[], y2[]: node coefficients at k+1 and k
f[], fp[]: output function values and derivatives
*/
void agbnp3_cspline_interpolate_soa(float *kv, float *xh, float dx, int m, 
				    float* yp, float *y,
				    float* y2p, float *y2,
				    float *f, float *fp){
  int i;
  float b, a, a2, b2, kf, dp1, dp2, yk, ypk, y2k, y2pk, dxinv;
  
  dp1 = dx/6.0f;
  dp2 = dx*dp1;
  dxinv = 1.f/dx;

#pragma vector aligned
#pragma ivdep
  for(i=0;i<m;i++){
    kf=kv[i];
    yk = y[i];
    ypk = yp[i];
    y2k = y2[i];
    y2pk = y2p[i];

    a = (kf+1.0f)-xh[i];
    a2 = a*a;
    b = xh[i] - kf;
    b2 = b*b;

    f[i] = a*yk+b*ypk +  ((a2*a - a)*y2k + (b2*b - b)*y2pk)*dp2;
    fp[i] = (ypk - yk)*dxinv - 
      ((3.0f*a2 - 1.f)*y2k + (3.0f*b2 - 1.f)*y2pk)*dp1;
  }
}

int agbnp3_i4p_soa(AGBNPdata *agb, float* rij, float *Ri, float *Rj, 
		   int m, float *f, float *fp,
		   float *mbuffera, float *mbufferb,
		   float *qkv, float *qxh, float *qyp, float *qy, float *qy2p, float *qy2,
		   float *qf1, float *qf2, float *qfp1, float *qfp2){
  int i;
  float_a rj1, u, ainv, ainv2;
  float *a = mbuffera;
  float *b = mbufferb;

#pragma vector aligned
#pragma ivdep
  for(i=0;i<m;i++){
    a[i] = rij[i]/Rj[i];
    b[i] = Ri[i]/Rj[i];
    //printf("i4p: %d %f %f\n", i, a[i], b[i]);
  }

  agbnp3_interpolate_ctablef42d_soa(agb->f4c1table2dh, a, b, m, f, fp,
	               qkv, qxh, qyp, qy, qy2p, qy2, qf1, qf2, qfp1, qfp2);

#pragma vector aligned
#pragma ivdep
  for(i=0;i<m;i++){
    f[i] = f[i]/Rj[i];
    fp[i] = fp[i]/(Rj[i]*Rj[i]);
  }

  return AGBNP_OK;
}



/* vectorized form of  agbnp3_interpolate_ctablef42d() */
int agbnp3_interpolate_ctablef42d_soa
(C1Table2DH *table2d, float *x, float *ym, int m, float *f, float *fp,
 float *kv, float *xh, float *yp, float *y, float *y2p, float *y2,
 float *f1, float *f2, float *fp1, float *fp2){

  int i, iy, k, key;
  float dy, dyinv, dx, dxinv, yn;
  float a, b;
  C1Table *table1, *table2;
  int nkey = table2d->nkey;

  for(i=0;i<m;i++){

    key = ym[i] * nkey;
    iy = agbnp3_h_find(table2d->y2i, key); 
    table1 = table2d->table[iy];

    dx = table1->dx;
    dxinv = table1->dxinv;
    
    xh[i] = x[i]*dxinv;
    k = xh[i];
    kv[i] = k;
    if(k > table1->n-2){
      y[i] = 0.0;
      yp[i] = 0.0;
      y2[i] = 0.0;
      y2p[i] = 0.0;
    }else{
      y[i] = table1->y[k];
      yp[i] = table1->y[k+1];
      y2[i] = table1->y2[k];
      y2p[i] = table1->y2[k+1];
    }
  }

#ifdef USE_SSE
  agbnp3_cspline_interpolate_ps(kv, xh, dx, m, 
				 yp, y,
				 y2p, y2,
				 f, fp);
#else
  agbnp3_cspline_interpolate_soa(kv, xh, dx, m, 
				 yp, y,
				 y2p, y2,
				 f, fp);
#endif

  return AGBNP_OK;
}


int agbnp3_create_ctablef4(int n, float_a amax, float_a b, 
			  C1Table **c1table){
  C1Table *tbl;
  int i;
  float_a da = amax/(n-1);
  float_a a, u1, q1, dr, Rj=1.0, fp, fpp, s;

  float_a *y, *y2, yp1, ypn = 0.0;
  float_a yinf=0.0;


  agbnp3_memalloc((void **)&(y), n*sizeof(float));
  agbnp3_memalloc((void **)&(y2), n*sizeof(float));
  if(!(y && y2)){
    agbnp3_errprint( "agbnp3_create_ctablef4(): unable to allocate work buffers (%d floats)\n", 3*n);
    return AGBNP_ERR;
  }

  tbl = malloc(sizeof(C1Table));
  if(!tbl){
    agbnp3_errprint( "agbnp3_create_ctablef4(): unable to allocate table structure.\n");
    return AGBNP_ERR;
  }
  tbl->n = n;
  tbl->dx = da;
  tbl->dxinv = 1./da;
  tbl->yinf = yinf;

  a = 0.0;
  for(i=0;i<n-1;i++){
    q1 = agbnp3_i4(a,b,Rj,&dr);
    if(i==0) yp1 = dr;
    y[i] = q1;
    a += da;
  }
  y[n-1] = yinf;

  agbnp3_cspline_setup(da, n, y, yp1, ypn, y2);
  tbl->y = y;
  tbl->y2 = y2;

  *c1table = tbl;
  return AGBNP_OK;
}


int agbnp3_interpolate_ctable(C1Table *c1table, float_a x, 
			     float_a *f, float_a *fp){
  int n = c1table->n;
  float dx = c1table->dx;
  float *y = c1table->y;
  float *y2 = c1table->y2;
 
  agbnp3_cspline_interpolate(x, dx, n, y, y2, f, fp);

  return AGBNP_OK;
}



void agbnp3_test_cspline(void){
  float x, dx = 0.2;
  int n = 100;
  float dxi = 0.02;
  int ni = 1000;
  float *y = malloc(n*sizeof(float));
  float *y2 = malloc(n*sizeof(float));
  float yp1, ypn = 0.0;
  float f, fp;
  int i;
  float Rj = 2.0, Ri = 1.0, dq;
  float q;
  float fold, xold;

  x = 0.0;
  for(i=0;i<n;i++){
    y[i] = agbnp3_i4(x, Ri, Rj, &dq);
    if(i==0) yp1 = dq;
    x += dx;
  }
  //  yp1 = 3.0;
  agbnp3_cspline_setup(dx, n, y, yp1, ypn, y2);

  x = 0.0;
  for(i=0;i<ni;i++){
    agbnp3_cspline_interpolate(x, dx, n, y, y2, &f, &fp);
    //q = agbnp3_i4(x, Ri, Rj, &dq);
    if(i>0){
      printf("csp: %f %f %f %f\n", x, f, f-fold, fp*dxi);
    }
    fold = f;
    xold = x;
    x += dxi;
  }
  free(y);
  free(y2);
}

int agbnp3_reset_buffers(AGBNPdata *agb, AGBworkdata *agbw_h){
  
  int natoms = agb->natoms;
  int nheavyat = agb->nheavyat;
  AGBworkdata *agbw = agb->agbw;
  float *r = agb->r;
  float *vols = agbw_h->vols;
  int i, iat;
  float cvdw = AGBNP_RADIUS_INCREMENT;

#ifdef _OPENMP
  memset(agbw_h->volumep,0,natoms*sizeof(float));
  memset(agbw_h->surf_area,0,natoms*sizeof(float));
  memset(agbw_h->br1,0,natoms*sizeof(float));
#endif

#pragma omp single nowait
  {
    memset(agbw->volumep,0,natoms*sizeof(float));
    for(iat=0;iat<nheavyat;iat++){
      agbw->volumep[iat] = vols[iat];
    }
  }
#pragma omp single nowait
  {
    memset(agbw->surf_area,0,natoms*sizeof(float));
    for(iat=0;iat<nheavyat;iat++){
      agbw->surf_area[iat] = 4.*pi*r[iat]*r[iat];
    }
  }
#pragma omp single nowait
  {
    for(i=0;i<natoms;i++){
      memset(agbw->mvpji[i],0,natoms*sizeof(float));
    }
  }
#pragma omp single nowait
  {
    for(iat=0;iat<natoms;iat++){
      agbw->br1[iat] = 1./(r[iat]-cvdw);
    }
  }
#pragma omp single
  {
    agb->ehb = 0.0;
  }

  return AGBNP_OK;
}


/* computes volume scaling factors, also adds surface area corrections to
   self volumes. */
 int agbnp3_scaling_factors(AGBNPdata *agb, AGBworkdata *agbw_h){
  int i,iat;
  int natoms = agb->natoms;
  int nheavyat = agb->nheavyat;
  float_a *r = agb->r;
  AGBworkdata *agbw = agb->agbw;
  float_a *volumep = agbw->volumep;
  float_a *vols = agbw->vols;
  float_a *sp = agbw->sp;
  float_a *spe = agbw->spe;
  float_a *surf_area = agbw->surf_area;
  float_a *surf_area_f = agbw->surf_area_f;

  float_a *volumep_h = agbw_h->volumep;
  float_a *surf_area_h = agbw_h->surf_area;
  float_a *sp_h = agbw_h->sp;
  float_a *spe_h = agbw_h->spe;
  float_a *psvol_h = agbw_h->psvol;
  float_a *vols_h = agbw_h->vols;
  float_a Rw = AGBNP_RADIUS_INCREMENT;
  float_a rvdw, us, pr;
  float_a a, f, fp;

#ifdef _OPENMP
  // threads contributions to master
#pragma omp critical
  for(iat=0;iat<nheavyat;iat++){
      volumep[iat] +=  volumep_h[iat];
  }
#pragma omp critical
  for(iat=0;iat<nheavyat;iat++){
    surf_area[iat] +=  surf_area_h[iat];
  }
#pragma omp barrier
#endif

#pragma omp single
  {
    /* filters surface areas to avoid negative surface areas */
    memset(agb->surf_area,0,natoms*sizeof(float_i));
    memset(agb->agbw->surf_area_f,0,natoms*sizeof(float_a));
    for(iat=0;iat<nheavyat;iat++){
      a = agbw->surf_area[iat];
      f = agbnp3_swf_area(a, &fp);
      agb->surf_area[iat] = agbw->surf_area[iat]*f;
      surf_area_f[iat] = agb->surf_area[iat];
      agbw->gammap[iat] = agbw->gamma[iat]*(f+a*fp);
    }
  }

  /* compute scaled volume factors for enlarged atomic radii, 
     that is before subtracting subtended surface area */
  for(iat=0;iat<nheavyat;iat++){
    spe_h[iat] = volumep[iat]/vols_h[iat];
  }

  /* subtract volume subtended by surface area:
     V = A (1/3) R2 [ 1 - (R1/R2)^3 ] 
     where R2 is the enlarged radius and R1 the vdw radius
  */
  for(iat=0;iat<nheavyat;iat++){
    rvdw = agb->r[iat] - Rw;
    a = surf_area[iat];
    f = agbnp3_swf_area(a, &fp);
    pr = r[iat]*(1. - pow(rvdw/r[iat],3))/3.;
    us = pr;
    psvol_h[iat] = (fp*a+f)*us; /* needed to compute effective gamma's in
				       agbnp3_gb_deruv() */
    volumep_h[iat] = volumep[iat] - surf_area_f[iat]*us;
  }

  /* compute scaled volume factors */
  for(iat=0;iat<nheavyat;iat++){
    sp_h[iat] = volumep_h[iat]/vols_h[iat];
  }

#ifdef _OPENMP
  //sync master copies
  // the barrier is needed because volumep is used above by lagging threads
#pragma omp barrier
#pragma omp single
  {
    memcpy(volumep,  volumep_h,nheavyat*sizeof(float));
    memcpy(spe,      spe_h,    nheavyat*sizeof(float));
    memcpy(sp,       sp_h,     nheavyat*sizeof(float));
  }
#endif

  return AGBNP_OK;
}

 int agbnp3_born_radii(AGBNPdata *agb, AGBworkdata *agbw_h){
  int natoms = agb->natoms;
  int iat;
  float_a fp;
  float_a *r = agb->r;
  AGBworkdata *agbw = agb->agbw;
  float_a *br = agbw->br;
  float_a *br1 = agbw->br1;
  float_a *br1_swf_der = agbw->br1_swf_der;
  float_a *brw = agbw->brw;
  float_a *brw_h = agbw_h->brw;
  float_a *br1_swf_der_h = agbw_h->br1_swf_der;
  float_a *br1_h = agbw_h->br1;
  float_a *br_h = agbw_h->br;
  float_a rw = 1.4;  /* water radius offset for np 
				    energy function */
  float_a _agbnp3_brw1, _agbnp3_brw2, _agbnp3_brw3; 
  float_a cvdw = AGBNP_RADIUS_INCREMENT;
  float_a biat;

#ifdef _OPENMP
  // add thread contributions to master copy
#pragma omp critical
  for(iat=0;iat<natoms;iat++){
    br1[iat] += br1_h[iat];
  }
#pragma omp barrier
#endif

  // now all threads compute born radii etc from master copy
  for(iat = 0; iat < natoms ; iat++){
    /* filters out very large or, worse, negative born radii */
    br1_h[iat] = agbnp3_swf_invbr(br1[iat], &fp);
    /* save derivative of filter function for later use */
    br1_swf_der_h[iat] = fp;
    /* calculates born radius from inverse born radius */
    br_h[iat] = 1./br1_h[iat];
    biat = br_h[iat];
    brw_h[iat] = AGBNP_BRW(biat,rw); /* 3*b^2/(b+rw)^4 for np derivative */
  }

#ifdef _OPENMP
  //barrier is needed because br1 master is used above by lagging threads
#pragma omp barrier
#pragma omp single nowait
  { memcpy(br1,         br1_h,       natoms*sizeof(float)); }
#pragma omp single nowait
  {  memcpy(br,          br_h,        natoms*sizeof(float)); }
#pragma omp single nowait
  {  memcpy(br1_swf_der, br1_swf_der_h, natoms*sizeof(float)); }
#pragma omp single
  {  memcpy(brw,         brw_h,       natoms*sizeof(float)); }
#endif

  return AGBNP_OK;
}


 int agbnp3_reset_derivatives(AGBNPdata *agb, AGBworkdata *agbw_h){
  int natoms = agb->natoms;

#ifdef _OPENMP
  memset(agbw_h->dgbdr_h,0,3*natoms*sizeof(float_a));
  memset(agbw_h->dvwdr_h,0,3*natoms*sizeof(float_a));
  memset(agbw_h->decav_h,0,3*natoms*sizeof(float_a));
  memset(agbw_h->dehb,0,3*natoms*sizeof(float_a));
  memset(agbw_h->dera,0,natoms*sizeof(float_a));
  memset(agbw_h->deru,0,natoms*sizeof(float_a));
  memset(agbw_h->derv,0,natoms*sizeof(float_a));
  memset(agbw_h->derus,0,natoms*sizeof(float_a));
  memset(agbw_h->dervs,0,natoms*sizeof(float_a));
  memset(agbw_h->derh,0,natoms*sizeof(float_a));
#endif

#pragma omp single nowait
  {memset(agb->agbw->dgbdr_h,0,3*natoms*sizeof(float_a)); }
#pragma omp single nowait
  {memset(agb->agbw->dvwdr_h,0,3*natoms*sizeof(float_a));}
#pragma omp single nowait
  {memset(agb->agbw->decav_h,0,3*natoms*sizeof(float_a));}
#pragma omp single nowait
  {memset(agb->agbw->dehb,0,3*natoms*sizeof(float_a));}
#pragma omp single nowait
  {memset(agb->agbw->dera,0,natoms*sizeof(float_a));}
#pragma omp single nowait
  {memset(agb->agbw->deru,0,natoms*sizeof(float_a));}
#pragma omp single nowait
  {memset(agb->agbw->derv,0,natoms*sizeof(float_a));}
#pragma omp single nowait
  {memset(agb->agbw->derus,0,natoms*sizeof(float_a));}
#pragma omp single nowait
  {memset(agb->agbw->dervs,0,natoms*sizeof(float_a));}
#pragma omp single
  {memset(agb->agbw->derh,0,natoms*sizeof(float_a));}

  return AGBNP_OK;
}

/* hash table functions */

unsigned int agbnp3_two2n_size(unsigned int m){
  /* returns smallest power of 2 larger than twice the input */
  unsigned int s = 1;
  unsigned int l = 2*m;
  if(m<=0) return 0;
  while(s<l){
    s = (s<<1);
  }
  return s;
}

HTable *agbnp3_h_create(int nat, int size, int jump){
  HTable *ht = NULL;

#pragma omp critical
  ht = (HTable *)calloc(1,sizeof(HTable));
  if(!ht) return ht;
  if(size <= 0) return ht;

  ht->hsize = size;
  ht->hmask = size - 1;
  ht->hjump = jump;
  ht->nat = nat;
#pragma omp critical
  ht->key =  (int *)malloc(size*sizeof(int));
  if(!ht->key){
    h_delete(ht);
    ht = NULL;
    return ht;
  }
  return ht;
}

void agbnp3_h_delete(HTable *ht){
  if(ht){
    if(ht->key) agbnp2_free(ht->key);
    agbnp2_free(ht);
  }
}

void agbnp3_h_init(HTable *ht){
  int i;
  int *key = ht->key;
  for(i=0;i<ht->hsize;i++){
    key[i] = -1;
  }
}

/* find a slot or return existing slot */
int agbnp3_h_enter(HTable *ht, unsigned int keyij){
  unsigned int hmask;
  unsigned int hjump;
  unsigned int k;
  int *key;
  if(!ht) return -1;
  hmask = ht->hmask;
  hjump = ht->hjump;
  key = ht->key;
  if(!key) return -1;
  k = (keyij & hmask);
  while(key[k] >= 0 && key[k] != keyij){
    k = ( (k + hjump) & hmask);
  }
  key[k] = keyij;
  return k;
}

/* return existing slot, or -1 if not found */
int agbnp3_h_find(HTable *ht, unsigned int keyij){
  unsigned int hmask;
  unsigned int hjump;
  unsigned int k;
  int *key;
  if(!ht) return -1;
  hmask = ht->hmask;
  hjump = ht->hjump;
  key = ht->key;
  if(!key) return -1;
  k = (keyij & hmask);
  while(key[k] >= 0 &&  key[k] !=  keyij){
    k = ( (k+hjump) & hmask);
  }
  if(key[k] < 0) return -1;
  return k;
}

/*                                                          *
 *    Functions to create/manage q4 look-up tables          *
 *                                                          */

/* get the number and list of "radius types"
   it is the responsibility of the caller to free radii array
 */
int agbnp3_list_radius_types(AGBNPdata *agb, float **radii){
  float *radius = (float *)malloc(agb->natoms*sizeof(float));
  int ntypes = 0;
  float riat;
  int i, iat, found;

  for(iat=0;iat<agb->natoms;iat++){
    
    riat = agb->r[iat];

    /* search list of radii */
    found = 0;
    for(i=0;i<ntypes;i++){
      if( fabs(riat-radius[i]) < FLT_MIN ){
	found = 1;
	break;
      }
    }

    /* if not found add it, increment set size */
    if(!found){
      radius[ntypes++] = riat;
    }

  }

  *radii = radius;
  return ntypes;
}

int agbnp3_create_ctablef42d_hash(AGBNPdata *agb, int na, float_a amax, 
				  C1Table2DH **table2d){
  C1Table2DH *tbl2d;
  float b;
  int size;
  float *radii;
  int nlookup;
  int i, j;
  int ntypes, key, slot;
  float c = AGBNP_RADIUS_INCREMENT;
  int nkey;

  tbl2d = malloc(sizeof(C1Table2DH));

  /* get list of radii */
  ntypes = agbnp3_list_radius_types(agb, &radii);

  /* number of look-up tables is ~ntypes^2 */
  size = agbnp3_two2n_size(ntypes*ntypes);
  tbl2d->table = (C1Table **)malloc(size*sizeof(C1Table *));
  tbl2d->y2i = agbnp3_h_create(0, size, 1);
  agbnp3_h_init(tbl2d->y2i);

  /* set multiplicative factor for key */
  tbl2d->nkey = 10000;
  nkey = tbl2d->nkey;

  /* now loop over all possible combinations of radii and constructs look up table for each */
  for(i=0;i<ntypes;i++){
    for(j=0;j<ntypes;j++){

      b = (radii[i]-c)/radii[j];
      key = b * nkey;
      slot = agbnp3_h_enter(tbl2d->y2i, key);
      if(agbnp3_create_ctablef4(na,amax,b,&(tbl2d->table[slot]))!=AGBNP_OK){
	agbnp3_errprint( "agbnp3_create_ctablef42d_hash(): error in agbnp3_create_ctablef4()\n");
	free(radii);
	return AGBNP_ERR;
      }

    }
  }

  free(radii);
  *table2d = tbl2d;
  return AGBNP_OK;
}


/* for a random pair of atoms, print the Q4 function */
int agbnp3_test_create_ctablef42d_hash(AGBNPdata *agb, float amax, C1Table2DH *table2d){
  int iat = 271;
  int jat = 12;
  int key, slot;
  float b;
  C1Table *table;
  float c = AGBNP_RADIUS_INCREMENT;
  float x, f, fp;
  int nkey = table2d->nkey;

  b = (agb->r[iat]-c)/agb->r[jat];
  key = b * nkey;
  slot = agbnp3_h_find(table2d->y2i, key);
  if(slot < 0){
    agbnp3_errprint( "agbnp3_test_create_ctablef42d_hash(): unable to find entry for radii combination (%f,%f)\n", agb->r[iat], agb->r[jat]);
    return AGBNP_ERR;
  }

  table = table2d->table[slot];


  {
    int i;
    printf("IntHash: radius: %f %f %f\n", b, agb->r[iat], agb->r[jat]);
    for(i = 0; i < 100; i++){
      x = 0.1*(float)i + 0.0001;
      agbnp3_interpolate_ctable(table, x, &f, &fp);
      printf("IntHash: radius: %f %f\n",x, f);
    }
  }
  
  

  return AGBNP_OK;
}


