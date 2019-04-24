/*
 * This file is part of pocketfft.
 * Licensed under a 3-clause BSD style license - see LICENSE.md
 */

/*
 *  Main implementation file.
 *
 *  Copyright (C) 2004-2019 Max-Planck-Society
 *  \author Martin Reinecke
 */

#include <cmath>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

#ifdef __GNUC__
#define NOINLINE __attribute__((noinline))
#define restrict __restrict__
#else
#define NOINLINE
#define restrict
#endif

using namespace std;

namespace {

template<typename T> struct arr
  {
  private:
    T *p;
    size_t sz;

    static T *ralloc(size_t num)
      { return (T *)aligned_alloc(64,num*sizeof(T)); }
    static void dealloc(T *ptr)
      { free(ptr); }

  public:
    arr() : p(0), sz(0) {}
    arr(size_t n) : p(ralloc(n)), sz(n)
      {
      if ((!p) && (n!=0))
        throw bad_alloc();
      }
    arr(arr &&other)
      : p(other.p), sz(other.sz)
      { other.p=nullptr; other.sz=0; }
    ~arr() { dealloc(p); }

    void resize(size_t n)
      {
      if (n==sz) return;
      dealloc(p);
      p = ralloc(n);
      sz = n;
      }

    T &operator[](size_t idx) { return p[idx]; }
    const T &operator[](size_t idx) const { return p[idx]; }

    T *data() { return p; }
    const T *data() const { return p; }

    size_t size() const { return sz; }
  };

//
// twiddle factor section
//

class sincos_2pibyn
  {
  private:
    arr<double> data;

    // adapted from https://stackoverflow.com/questions/42792939/
    // CAUTION: this function only works for arguments in the range
    //          [-0.25; 0.25]!
    void my_sincosm1pi (double a, double *restrict res)
      {
      double s = a * a;
      /* Approximate cos(pi*x)-1 for x in [-0.25,0.25] */
      double r =     -1.0369917389758117e-4;
      r = fma (r, s,  1.9294935641298806e-3);
      r = fma (r, s, -2.5806887942825395e-2);
      r = fma (r, s,  2.3533063028328211e-1);
      r = fma (r, s, -1.3352627688538006e+0);
      r = fma (r, s,  4.0587121264167623e+0);
      r = fma (r, s, -4.9348022005446790e+0);
      double c = r*s;
      /* Approximate sin(pi*x) for x in [-0.25,0.25] */
      r =             4.6151442520157035e-4;
      r = fma (r, s, -7.3700183130883555e-3);
      r = fma (r, s,  8.2145868949323936e-2);
      r = fma (r, s, -5.9926452893214921e-1);
      r = fma (r, s,  2.5501640398732688e+0);
      r = fma (r, s, -5.1677127800499516e+0);
      s = s * a;
      r = r * s;
      s = fma (a, 3.1415926535897931e+0, r);
      res[0] = c;
      res[1] = s;
      }

    NOINLINE void calc_first_octant(size_t den, double * restrict res)
      {
      size_t n = (den+4)>>3;
      if (n==0) return;
      res[0]=1.; res[1]=0.;
      if (n==1) return;
      size_t l1=(size_t)sqrt(n);
      for (size_t i=1; i<l1; ++i)
        my_sincosm1pi((2.*i)/den,&res[2*i]);
      size_t start=l1;
      while(start<n)
        {
        double cs[2];
        my_sincosm1pi((2.*start)/den,cs);
        res[2*start] = cs[0]+1.;
        res[2*start+1] = cs[1];
        size_t end = l1;
        if (start+end>n) end = n-start;
        for (size_t i=1; i<end; ++i)
          {
          double csx[2]={res[2*i], res[2*i+1]};
          res[2*(start+i)] = ((cs[0]*csx[0] - cs[1]*csx[1] + cs[0]) + csx[0]) + 1.;
          res[2*(start+i)+1] = (cs[0]*csx[1] + cs[1]*csx[0]) + cs[1] + csx[1];
          }
        start += l1;
        }
      for (size_t i=1; i<l1; ++i)
        res[2*i] += 1.;
      }

    NOINLINE void calc_first_quadrant(size_t n, double * restrict res)
      {
      double * restrict p = res+n;
      calc_first_octant(n<<1, p);
      size_t ndone=(n+2)>>2;
      size_t i=0, idx1=0, idx2=2*ndone-2;
      for (; i+1<ndone; i+=2, idx1+=2, idx2-=2)
        {
        res[idx1]   = p[2*i];
        res[idx1+1] = p[2*i+1];
        res[idx2]   = p[2*i+3];
        res[idx2+1] = p[2*i+2];
        }
      if (i!=ndone)
        {
        res[idx1  ] = p[2*i];
        res[idx1+1] = p[2*i+1];
        }
      }

    NOINLINE void calc_first_half(size_t n, double * restrict res)
      {
      int ndone=(n+1)>>1;
      double * p = res+n-1;
      calc_first_octant(n<<2, p);
      int i4=0, in=n, i=0;
      for (; i4<=in-i4; ++i, i4+=4) // octant 0
        {
        res[2*i] = p[2*i4]; res[2*i+1] = p[2*i4+1];
        }
      for (; i4-in <= 0; ++i, i4+=4) // octant 1
        {
        int xm = in-i4;
        res[2*i] = p[2*xm+1]; res[2*i+1] = p[2*xm];
        }
      for (; i4<=3*in-i4; ++i, i4+=4) // octant 2
        {
        int xm = i4-in;
        res[2*i] = -p[2*xm+1]; res[2*i+1] = p[2*xm];
        }
      for (; i<ndone; ++i, i4+=4) // octant 3
        {
        int xm = 2*in-i4;
        res[2*i] = -p[2*xm]; res[2*i+1] = p[2*xm+1];
        }
      }

    NOINLINE void fill_first_quadrant(size_t n, double * restrict res)
      {
      const double hsqt2 = 0.707106781186547524400844362104849;
      size_t quart = n>>2;
      if ((n&7)==0)
        res[quart] = res[quart+1] = hsqt2;
      for (size_t i=2, j=2*quart-2; i<quart; i+=2, j-=2)
        {
        res[j  ] = res[i+1];
        res[j+1] = res[i  ];
        }
      }

    NOINLINE void fill_first_half(size_t n, double * restrict res)
      {
      size_t half = n>>1;
      if ((n&3)==0)
        for (size_t i=0; i<half; i+=2)
          {
          res[i+half]   = -res[i+1];
          res[i+half+1] =  res[i  ];
          }
      else
        for (size_t i=2, j=2*half-2; i<half; i+=2, j-=2)
          {
          res[j  ] = -res[i  ];
          res[j+1] =  res[i+1];
          }
      }

    NOINLINE void fill_second_half(size_t n, double * restrict res)
      {
      if ((n&1)==0)
        for (size_t i=0; i<n; ++i)
          res[i+n] = -res[i];
      else
        for (size_t i=2, j=2*n-2; i<n; i+=2, j-=2)
          {
          res[j  ] =  res[i  ];
          res[j+1] = -res[i+1];
          }
      }

    NOINLINE void sincos_2pibyn_half(size_t n, double * restrict res)
      {
      if ((n&3)==0)
        {
        calc_first_octant(n, res);
        fill_first_quadrant(n, res);
        fill_first_half(n, res);
        }
      else if ((n&1)==0)
        {
        calc_first_quadrant(n, res);
        fill_first_half(n, res);
        }
      else
        calc_first_half(n, res);
      }

  public:
    sincos_2pibyn(size_t n, bool half)
      : data(2*n)
      {
      sincos_2pibyn_half(n, data.data());
      if (!half) fill_second_half(n, data.data());
      }

    double operator[](size_t idx) const { return data[idx]; }
  };

NOINLINE size_t largest_prime_factor (size_t n)
  {
  size_t res=1;
  while ((n&1)==0)
    { res=2; n>>=1; }

  size_t limit=size_t(sqrt(n+0.01));
  for (size_t x=3; x<=limit; x+=2)
  while ((n/x)*x==n)
    {
    res=x;
    n/=x;
    limit=size_t(sqrt(n+0.01));
    }
  if (n>1) res=n;

  return res;
  }

NOINLINE double cost_guess (size_t n)
  {
  const double lfp=1.1; // penalty for non-hardcoded larger factors
  size_t ni=n;
  double result=0.;
  while ((n&1)==0)
    { result+=2; n>>=1; }

  size_t limit=size_t(sqrt(n+0.01));
  for (size_t x=3; x<=limit; x+=2)
  while ((n/x)*x==n)
    {
    result+= (x<=5) ? x : lfp*x; // penalize larger prime factors
    n/=x;
    limit=size_t(sqrt(n+0.01));
    }
  if (n>1) result+=(n<=5) ? n : lfp*n;

  return result*ni;
  }

/* returns the smallest composite of 2, 3, 5, 7 and 11 which is >= n */
NOINLINE size_t good_size(size_t n)
  {
  if (n<=12) return n;

  size_t bestfac=2*n;
  for (size_t f2=1; f2<bestfac; f2*=2)
    for (size_t f23=f2; f23<bestfac; f23*=3)
      for (size_t f235=f23; f235<bestfac; f235*=5)
        for (size_t f2357=f235; f2357<bestfac; f2357*=7)
          for (size_t f235711=f2357; f235711<bestfac; f235711*=11)
            if (f235711>=n) bestfac=f235711;
  return bestfac;
  }

template<typename T> struct cmplx {
  T r, i;
  cmplx() {}
  cmplx(T r_, T i_) : r(r_), i(i_) {}
  void Set(T r_, T i_) { r=r_; i=i_; }
  void Set(T r_) { r=r_; i=T(0); }
  cmplx &operator+= (const cmplx &other)
    { r+=other.r; i+=other.i; return *this; }
  template<typename T2>cmplx &operator*= (T2 other)
    { r*=other; i*=other; return *this; }
  cmplx operator+ (const cmplx &other) const
    { return cmplx(r+other.r, i+other.i); }
  cmplx operator- (const cmplx &other) const
    { return cmplx(r-other.r, i-other.i); }
  template<typename T2> auto operator* (const T2 &other) const
    -> cmplx<decltype(r*other)>
    {
    return {r*other, i*other};
    }
  template<typename T2> auto operator* (const cmplx<T2> &other) const
    -> cmplx<decltype(r+other.r)>
    {
    return {r*other.r-i*other.i, r*other.i + i*other.r};
    }
  template<bool bwd, typename T2> auto special_mul (const cmplx<T2> &other) const
    -> cmplx<decltype(r+other.r)>
    {
    if (bwd)
      return {r*other.r-i*other.i, r*other.i + i*other.r};
    else
      return {r*other.r+i*other.i, i*other.r - r*other.i};
    }
};
template<typename T> void PMC(cmplx<T> &a, cmplx<T> &b,
  const cmplx<T> &c, const cmplx<T> &d)
  { a = c+d; b = c-d; }
template<typename T> cmplx<T> conj(const cmplx<T> &a)
  { return {a.r, -a.i}; }

template<typename T> void ROT90(cmplx<T> &a)
  { auto tmp_=a.r; a.r=-a.i; a.i=tmp_; }
template<typename T> void ROTM90(cmplx<T> &a)
  { auto tmp_=-a.r; a.r=a.i; a.i=tmp_; }

#define CH(a,b,c) ch[(a)+ido*((b)+l1*(c))]
#define CC(a,b,c) cc[(a)+ido*((b)+cdim*(c))]
#define WA(x,i) wa[(i)-1+(x)*(ido-1)]

constexpr size_t NFCT=25;

//
// complex FFTPACK transforms
//

template<typename T0> class cfftp
  {
  private:

    struct fctdata
      {
      size_t fct;
      cmplx<T0> *tw, *tws;
      };

    size_t length, nfct;
    arr<cmplx<T0>> mem;
    fctdata fct[NFCT];

    void add_factor(size_t factor)
      {
      if (nfct>=NFCT) throw runtime_error("too many prime factors");
      fct[nfct++].fct = factor;
      }

template<bool bwd, typename T> NOINLINE void pass2 (size_t ido, size_t l1,
  const T * restrict cc, T * restrict ch, const cmplx<T0> * restrict wa)
  {
  constexpr size_t cdim=2;

  if (ido==1)
    for (size_t k=0; k<l1; ++k)
      {
      CH(0,k,0) = CC(0,0,k)+CC(0,1,k);
      CH(0,k,1) = CC(0,0,k)-CC(0,1,k);
      }
  else
    for (size_t k=0; k<l1; ++k)
      {
      CH(0,k,0) = CC(0,0,k)+CC(0,1,k);
      CH(0,k,1) = CC(0,0,k)-CC(0,1,k);
      for (size_t i=1; i<ido; ++i)
        {
        CH(i,k,0) = CC(i,0,k)+CC(i,1,k);
        CH(i,k,1) = (CC(i,0,k)-CC(i,1,k)).template special_mul<bwd>(WA(0,i));
        }
      }
  }

#define PREP3(idx) \
        T t0 = CC(idx,0,k), t1, t2; \
        PMC (t1,t2,CC(idx,1,k),CC(idx,2,k)); \
        CH(idx,k,0)=t0+t1;
#define PARTSTEP3a(u1,u2,twr,twi) \
        { \
        T ca,cb; \
        ca=t0+t1*twr; \
        cb=t2*twi; ROT90(cb); \
        PMC(CH(0,k,u1),CH(0,k,u2),ca,cb) ;\
        }
#define PARTSTEP3b(u1,u2,twr,twi) \
        { \
        T ca,cb,da,db; \
        ca=t0+t1*twr; \
        cb=t2*twi; ROT90(cb); \
        PMC(da,db,ca,cb); \
        CH(i,k,u1) = da.template special_mul<bwd>(WA(u1-1,i)); \
        CH(i,k,u2) = db.template special_mul<bwd>(WA(u2-1,i)); \
        }
template<bool bwd, typename T> NOINLINE void pass3 (size_t ido, size_t l1,
  const T * restrict cc, T * restrict ch, const cmplx<T0> * restrict wa)
  {
  constexpr size_t cdim=3;
  constexpr T0 tw1r=-0.5, tw1i= (bwd ? 1.: -1.) * 0.86602540378443864676;

  if (ido==1)
    for (size_t k=0; k<l1; ++k)
      {
      PREP3(0)
      PARTSTEP3a(1,2,tw1r,tw1i)
      }
  else
    for (size_t k=0; k<l1; ++k)
      {
      {
      PREP3(0)
      PARTSTEP3a(1,2,tw1r,tw1i)
      }
      for (size_t i=1; i<ido; ++i)
        {
        PREP3(i)
        PARTSTEP3b(1,2,tw1r,tw1i)
        }
      }
  }

template<bool bwd, typename T> NOINLINE void pass4 (size_t ido, size_t l1,
  const T * restrict cc, T * restrict ch, const cmplx<T0> * restrict wa)
  {
  constexpr size_t cdim=4;

  if (ido==1)
    for (size_t k=0; k<l1; ++k)
      {
      T t1, t2, t3, t4;
      PMC(t2,t1,CC(0,0,k),CC(0,2,k));
      PMC(t3,t4,CC(0,1,k),CC(0,3,k));
      bwd ? ROT90(t4) : ROTM90(t4);
      PMC(CH(0,k,0),CH(0,k,2),t2,t3);
      PMC(CH(0,k,1),CH(0,k,3),t1,t4);
      }
  else
    for (size_t k=0; k<l1; ++k)
      {
      {
      T t1, t2, t3, t4;
      PMC(t2,t1,CC(0,0,k),CC(0,2,k));
      PMC(t3,t4,CC(0,1,k),CC(0,3,k));
      bwd ? ROT90(t4) : ROTM90(t4);
      PMC(CH(0,k,0),CH(0,k,2),t2,t3);
      PMC(CH(0,k,1),CH(0,k,3),t1,t4);
      }
      for (size_t i=1; i<ido; ++i)
        {
        T c2, c3, c4, t1, t2, t3, t4;
        T cc0=CC(i,0,k), cc1=CC(i,1,k),cc2=CC(i,2,k),cc3=CC(i,3,k);
        PMC(t2,t1,cc0,cc2);
        PMC(t3,t4,cc1,cc3);
        bwd ? ROT90(t4) : ROTM90(t4);
        cmplx<T0> wa0=WA(0,i), wa1=WA(1,i),wa2=WA(2,i);
        PMC(CH(i,k,0),c3,t2,t3);
        PMC(c2,c4,t1,t4);
        CH(i,k,1) = c2.template special_mul<bwd>(wa0);
        CH(i,k,2) = c3.template special_mul<bwd>(wa1);
        CH(i,k,3) = c4.template special_mul<bwd>(wa2);
        }
      }
  }

#define PREP5(idx) \
        T t0 = CC(idx,0,k), t1, t2, t3, t4; \
        PMC (t1,t4,CC(idx,1,k),CC(idx,4,k)); \
        PMC (t2,t3,CC(idx,2,k),CC(idx,3,k)); \
        CH(idx,k,0).r=t0.r+t1.r+t2.r; \
        CH(idx,k,0).i=t0.i+t1.i+t2.i;

#define PARTSTEP5a(u1,u2,twar,twbr,twai,twbi) \
        { \
        T ca,cb; \
        ca.r=t0.r+twar*t1.r+twbr*t2.r; \
        ca.i=t0.i+twar*t1.i+twbr*t2.i; \
        cb.i=twai*t4.r twbi*t3.r; \
        cb.r=-(twai*t4.i twbi*t3.i); \
        PMC(CH(0,k,u1),CH(0,k,u2),ca,cb); \
        }

#define PARTSTEP5b(u1,u2,twar,twbr,twai,twbi) \
        { \
        T ca,cb,da,db; \
        ca.r=t0.r+twar*t1.r+twbr*t2.r; \
        ca.i=t0.i+twar*t1.i+twbr*t2.i; \
        cb.i=twai*t4.r twbi*t3.r; \
        cb.r=-(twai*t4.i twbi*t3.i); \
        PMC(da,db,ca,cb); \
        CH(i,k,u1) = da.template special_mul<bwd>(WA(u1-1,i)); \
        CH(i,k,u2) = db.template special_mul<bwd>(WA(u2-1,i)); \
        }
template<bool bwd, typename T> NOINLINE void pass5 (size_t ido, size_t l1,
  const T * restrict cc, T * restrict ch, const cmplx<T0> * restrict wa)
  {
  constexpr size_t cdim=5;
  constexpr T0 tw1r= 0.3090169943749474241,
               tw1i= (bwd ? 1.: -1.) * 0.95105651629515357212,
               tw2r= -0.8090169943749474241,
               tw2i= (bwd ? 1.: -1.) * 0.58778525229247312917;

  if (ido==1)
    for (size_t k=0; k<l1; ++k)
      {
      PREP5(0)
      PARTSTEP5a(1,4,tw1r,tw2r,+tw1i,+tw2i)
      PARTSTEP5a(2,3,tw2r,tw1r,+tw2i,-tw1i)
      }
  else
    for (size_t k=0; k<l1; ++k)
      {
      {
      PREP5(0)
      PARTSTEP5a(1,4,tw1r,tw2r,+tw1i,+tw2i)
      PARTSTEP5a(2,3,tw2r,tw1r,+tw2i,-tw1i)
      }
      for (size_t i=1; i<ido; ++i)
        {
        PREP5(i)
        PARTSTEP5b(1,4,tw1r,tw2r,+tw1i,+tw2i)
        PARTSTEP5b(2,3,tw2r,tw1r,+tw2i,-tw1i)
        }
      }
  }

#define PREP7(idx) \
        T t1 = CC(idx,0,k), t2, t3, t4, t5, t6, t7; \
        PMC (t2,t7,CC(idx,1,k),CC(idx,6,k)); \
        PMC (t3,t6,CC(idx,2,k),CC(idx,5,k)); \
        PMC (t4,t5,CC(idx,3,k),CC(idx,4,k)); \
        CH(idx,k,0).r=t1.r+t2.r+t3.r+t4.r; \
        CH(idx,k,0).i=t1.i+t2.i+t3.i+t4.i;

#define PARTSTEP7a0(u1,u2,x1,x2,x3,y1,y2,y3,out1,out2) \
        { \
        T ca,cb; \
        ca.r=t1.r+x1*t2.r+x2*t3.r+x3*t4.r; \
        ca.i=t1.i+x1*t2.i+x2*t3.i+x3*t4.i; \
        cb.i=y1*t7.r y2*t6.r y3*t5.r; \
        cb.r=-(y1*t7.i y2*t6.i y3*t5.i); \
        PMC(out1,out2,ca,cb); \
        }
#define PARTSTEP7a(u1,u2,x1,x2,x3,y1,y2,y3) \
        PARTSTEP7a0(u1,u2,x1,x2,x3,y1,y2,y3,CH(0,k,u1),CH(0,k,u2))
#define PARTSTEP7(u1,u2,x1,x2,x3,y1,y2,y3) \
        { \
        T da,db; \
        PARTSTEP7a0(u1,u2,x1,x2,x3,y1,y2,y3,da,db) \
        CH(i,k,u1) = da.template special_mul<bwd>(WA(u1-1,i)); \
        CH(i,k,u2) = db.template special_mul<bwd>(WA(u2-1,i)); \
        }

template<bool bwd, typename T> NOINLINE void pass7(size_t ido, size_t l1,
  const T * restrict cc, T * restrict ch, const cmplx<T0> * restrict wa)
  {
  constexpr size_t cdim=7;
  constexpr T0 tw1r= 0.623489801858733530525,
               tw1i= (bwd ? 1. : -1.) * 0.7818314824680298087084,
               tw2r= -0.222520933956314404289,
               tw2i= (bwd ? 1. : -1.) * 0.9749279121818236070181,
               tw3r= -0.9009688679024191262361,
               tw3i= (bwd ? 1. : -1.) * 0.4338837391175581204758;

  if (ido==1)
    for (size_t k=0; k<l1; ++k)
      {
      PREP7(0)
      PARTSTEP7a(1,6,tw1r,tw2r,tw3r,+tw1i,+tw2i,+tw3i)
      PARTSTEP7a(2,5,tw2r,tw3r,tw1r,+tw2i,-tw3i,-tw1i)
      PARTSTEP7a(3,4,tw3r,tw1r,tw2r,+tw3i,-tw1i,+tw2i)
      }
  else
    for (size_t k=0; k<l1; ++k)
      {
      {
      PREP7(0)
      PARTSTEP7a(1,6,tw1r,tw2r,tw3r,+tw1i,+tw2i,+tw3i)
      PARTSTEP7a(2,5,tw2r,tw3r,tw1r,+tw2i,-tw3i,-tw1i)
      PARTSTEP7a(3,4,tw3r,tw1r,tw2r,+tw3i,-tw1i,+tw2i)
      }
      for (size_t i=1; i<ido; ++i)
        {
        PREP7(i)
        PARTSTEP7(1,6,tw1r,tw2r,tw3r,+tw1i,+tw2i,+tw3i)
        PARTSTEP7(2,5,tw2r,tw3r,tw1r,+tw2i,-tw3i,-tw1i)
        PARTSTEP7(3,4,tw3r,tw1r,tw2r,+tw3i,-tw1i,+tw2i)
        }
      }
  }

#define PREP11(idx) \
        T t1 = CC(idx,0,k), t2, t3, t4, t5, t6, t7, t8, t9, t10, t11; \
        PMC (t2,t11,CC(idx,1,k),CC(idx,10,k)); \
        PMC (t3,t10,CC(idx,2,k),CC(idx, 9,k)); \
        PMC (t4,t9 ,CC(idx,3,k),CC(idx, 8,k)); \
        PMC (t5,t8 ,CC(idx,4,k),CC(idx, 7,k)); \
        PMC (t6,t7 ,CC(idx,5,k),CC(idx, 6,k)); \
        CH(idx,k,0).r=t1.r+t2.r+t3.r+t4.r+t5.r+t6.r; \
        CH(idx,k,0).i=t1.i+t2.i+t3.i+t4.i+t5.i+t6.i;

#define PARTSTEP11a0(u1,u2,x1,x2,x3,x4,x5,y1,y2,y3,y4,y5,out1,out2) \
        { \
        T ca = t1 + t2*x1 + t3*x2 + t4*x3 + t5*x4 +t6*x5, \
          cb; \
        cb.i=y1*t11.r y2*t10.r y3*t9.r y4*t8.r y5*t7.r; \
        cb.r=-(y1*t11.i y2*t10.i y3*t9.i y4*t8.i y5*t7.i ); \
        PMC(out1,out2,ca,cb); \
        }
#define PARTSTEP11a(u1,u2,x1,x2,x3,x4,x5,y1,y2,y3,y4,y5) \
        PARTSTEP11a0(u1,u2,x1,x2,x3,x4,x5,y1,y2,y3,y4,y5,CH(0,k,u1),CH(0,k,u2))
#define PARTSTEP11(u1,u2,x1,x2,x3,x4,x5,y1,y2,y3,y4,y5) \
        { \
        T da,db; \
        PARTSTEP11a0(u1,u2,x1,x2,x3,x4,x5,y1,y2,y3,y4,y5,da,db) \
        CH(i,k,u1) = da.template special_mul<bwd>(WA(u1-1,i)); \
        CH(i,k,u2) = db.template special_mul<bwd>(WA(u2-1,i)); \
        }

template<bool bwd, typename T> NOINLINE void pass11 (size_t ido, size_t l1,
  const T * restrict cc, T * restrict ch, const cmplx<T0> * restrict wa)
  {
  const size_t cdim=11;
  const T0 tw1r =        0.8412535328311811688618,
           tw1i = (bwd ? 1. : -1.) * 0.5406408174555975821076,
           tw2r =        0.4154150130018864255293,
           tw2i = (bwd ? 1. : -1.) * 0.9096319953545183714117,
           tw3r =       -0.1423148382732851404438,
           tw3i = (bwd ? 1. : -1.) * 0.9898214418809327323761,
           tw4r =       -0.6548607339452850640569,
           tw4i = (bwd ? 1. : -1.) * 0.755749574354258283774,
           tw5r =       -0.9594929736144973898904,
           tw5i = (bwd ? 1. : -1.) * 0.2817325568414296977114;

  if (ido==1)
    for (size_t k=0; k<l1; ++k)
      {
      PREP11(0)
      PARTSTEP11a(1,10,tw1r,tw2r,tw3r,tw4r,tw5r,+tw1i,+tw2i,+tw3i,+tw4i,+tw5i)
      PARTSTEP11a(2, 9,tw2r,tw4r,tw5r,tw3r,tw1r,+tw2i,+tw4i,-tw5i,-tw3i,-tw1i)
      PARTSTEP11a(3, 8,tw3r,tw5r,tw2r,tw1r,tw4r,+tw3i,-tw5i,-tw2i,+tw1i,+tw4i)
      PARTSTEP11a(4, 7,tw4r,tw3r,tw1r,tw5r,tw2r,+tw4i,-tw3i,+tw1i,+tw5i,-tw2i)
      PARTSTEP11a(5, 6,tw5r,tw1r,tw4r,tw2r,tw3r,+tw5i,-tw1i,+tw4i,-tw2i,+tw3i)
      }
  else
    for (size_t k=0; k<l1; ++k)
      {
      {
      PREP11(0)
      PARTSTEP11a(1,10,tw1r,tw2r,tw3r,tw4r,tw5r,+tw1i,+tw2i,+tw3i,+tw4i,+tw5i)
      PARTSTEP11a(2, 9,tw2r,tw4r,tw5r,tw3r,tw1r,+tw2i,+tw4i,-tw5i,-tw3i,-tw1i)
      PARTSTEP11a(3, 8,tw3r,tw5r,tw2r,tw1r,tw4r,+tw3i,-tw5i,-tw2i,+tw1i,+tw4i)
      PARTSTEP11a(4, 7,tw4r,tw3r,tw1r,tw5r,tw2r,+tw4i,-tw3i,+tw1i,+tw5i,-tw2i)
      PARTSTEP11a(5, 6,tw5r,tw1r,tw4r,tw2r,tw3r,+tw5i,-tw1i,+tw4i,-tw2i,+tw3i)
      }
      for (size_t i=1; i<ido; ++i)
        {
        PREP11(i)
        PARTSTEP11(1,10,tw1r,tw2r,tw3r,tw4r,tw5r,+tw1i,+tw2i,+tw3i,+tw4i,+tw5i)
        PARTSTEP11(2, 9,tw2r,tw4r,tw5r,tw3r,tw1r,+tw2i,+tw4i,-tw5i,-tw3i,-tw1i)
        PARTSTEP11(3, 8,tw3r,tw5r,tw2r,tw1r,tw4r,+tw3i,-tw5i,-tw2i,+tw1i,+tw4i)
        PARTSTEP11(4, 7,tw4r,tw3r,tw1r,tw5r,tw2r,+tw4i,-tw3i,+tw1i,+tw5i,-tw2i)
        PARTSTEP11(5, 6,tw5r,tw1r,tw4r,tw2r,tw3r,+tw5i,-tw1i,+tw4i,-tw2i,+tw3i)
        }
      }
  }

#define CX(a,b,c) cc[(a)+ido*((b)+l1*(c))]
#define CX2(a,b) cc[(a)+idl1*(b)]
#define CH2(a,b) ch[(a)+idl1*(b)]

template<bool bwd, typename T> NOINLINE void passg (size_t ido, size_t ip,
  size_t l1, T * restrict cc, T * restrict ch, const cmplx<T0> * restrict wa,
  const cmplx<T0> * restrict csarr)
  {
  const size_t cdim=ip;
  size_t ipph = (ip+1)/2;
  size_t idl1 = ido*l1;

  arr<cmplx<T0>> wal(ip);
  wal[0] = cmplx<T0>(1., 0.);
  for (size_t i=1; i<ip; ++i)
    wal[i]=cmplx<T0>(csarr[i].r,bwd ? csarr[i].i : -csarr[i].i);

  for (size_t k=0; k<l1; ++k)
    for (size_t i=0; i<ido; ++i)
      CH(i,k,0) = CC(i,0,k);
  for (size_t j=1, jc=ip-1; j<ipph; ++j, --jc)
    for (size_t k=0; k<l1; ++k)
      for (size_t i=0; i<ido; ++i)
        PMC(CH(i,k,j),CH(i,k,jc),CC(i,j,k),CC(i,jc,k));
  for (size_t k=0; k<l1; ++k)
    for (size_t i=0; i<ido; ++i)
      {
      T tmp = CH(i,k,0);
      for (size_t j=1; j<ipph; ++j)
        tmp+=CH(i,k,j);
      CX(i,k,0) = tmp;
      }
  for (size_t l=1, lc=ip-1; l<ipph; ++l, --lc)
    {
    // j=0
    for (size_t ik=0; ik<idl1; ++ik)
      {
      CX2(ik,l).r = CH2(ik,0).r+wal[l].r*CH2(ik,1).r+wal[2*l].r*CH2(ik,2).r;
      CX2(ik,l).i = CH2(ik,0).i+wal[l].r*CH2(ik,1).i+wal[2*l].r*CH2(ik,2).i;
      CX2(ik,lc).r=-wal[l].i*CH2(ik,ip-1).i-wal[2*l].i*CH2(ik,ip-2).i;
      CX2(ik,lc).i=wal[l].i*CH2(ik,ip-1).r+wal[2*l].i*CH2(ik,ip-2).r;
      }

    size_t iwal=2*l;
    size_t j=3, jc=ip-3;
    for (; j<ipph-1; j+=2, jc-=2)
      {
      iwal+=l; if (iwal>ip) iwal-=ip;
      cmplx<T0> xwal=wal[iwal];
      iwal+=l; if (iwal>ip) iwal-=ip;
      cmplx<T0> xwal2=wal[iwal];
      for (size_t ik=0; ik<idl1; ++ik)
        {
        CX2(ik,l).r += CH2(ik,j).r*xwal.r+CH2(ik,j+1).r*xwal2.r;
        CX2(ik,l).i += CH2(ik,j).i*xwal.r+CH2(ik,j+1).i*xwal2.r;
        CX2(ik,lc).r -= CH2(ik,jc).i*xwal.i+CH2(ik,jc-1).i*xwal2.i;
        CX2(ik,lc).i += CH2(ik,jc).r*xwal.i+CH2(ik,jc-1).r*xwal2.i;
        }
      }
    for (; j<ipph; ++j, --jc)
      {
      iwal+=l; if (iwal>ip) iwal-=ip;
      cmplx<T0> xwal=wal[iwal];
      for (size_t ik=0; ik<idl1; ++ik)
        {
        CX2(ik,l).r += CH2(ik,j).r*xwal.r;
        CX2(ik,l).i += CH2(ik,j).i*xwal.r;
        CX2(ik,lc).r -= CH2(ik,jc).i*xwal.i;
        CX2(ik,lc).i += CH2(ik,jc).r*xwal.i;
        }
      }
    }

  // shuffling and twiddling
  if (ido==1)
    for (size_t j=1, jc=ip-1; j<ipph; ++j, --jc)
      for (size_t ik=0; ik<idl1; ++ik)
        {
        T t1=CX2(ik,j), t2=CX2(ik,jc);
        PMC(CX2(ik,j),CX2(ik,jc),t1,t2);
        }
  else
    {
    for (size_t j=1, jc=ip-1; j<ipph; ++j,--jc)
      for (size_t k=0; k<l1; ++k)
        {
        T t1=CX(0,k,j), t2=CX(0,k,jc);
        PMC(CX(0,k,j),CX(0,k,jc),t1,t2);
        for (size_t i=1; i<ido; ++i)
          {
          T x1, x2;
          PMC(x1,x2,CX(i,k,j),CX(i,k,jc));
          size_t idij=(j-1)*(ido-1)+i-1;
          CX(i,k,j) = x1.template special_mul<bwd>(wa[idij]);
          idij=(jc-1)*(ido-1)+i-1;
          CX(i,k,jc) = x2.template special_mul<bwd>(wa[idij]);
          }
        }
    }
  }

#undef CH2
#undef CX2
#undef CX

template<bool bwd, typename T> NOINLINE void pass_all(T c[], T0 fact)
  {
  if (length==1) { c[0]*=fact; return; }
  size_t l1=1;
  arr<T> ch(length);
  T *p1=c, *p2=ch.data();

  for(size_t k1=0; k1<nfct; k1++)
    {
    size_t ip=fct[k1].fct;
    size_t l2=ip*l1;
    size_t ido = length/l2;
    if     (ip==4)
      pass4<bwd> (ido, l1, p1, p2, fct[k1].tw);
    else if(ip==2)
      pass2<bwd>(ido, l1, p1, p2, fct[k1].tw);
    else if(ip==3)
      pass3<bwd> (ido, l1, p1, p2, fct[k1].tw);
    else if(ip==5)
      pass5<bwd> (ido, l1, p1, p2, fct[k1].tw);
    else if(ip==7)
      pass7<bwd> (ido, l1, p1, p2, fct[k1].tw);
    else if(ip==11)
      pass11<bwd> (ido, l1, p1, p2, fct[k1].tw);
    else
      {
      passg<bwd>(ido, ip, l1, p1, p2, fct[k1].tw, fct[k1].tws);
      swap(p1,p2);
      }
    swap(p1,p2);
    l1=l2;
    }
  if (p1!=c)
    {
    if (fact!=1.)
      for (size_t i=0; i<length; ++i)
        c[i] = ch[i]*fact;
    else
      memcpy (c,p1,length*sizeof(T));
    }
  else
    if (fact!=1.)
      for (size_t i=0; i<length; ++i)
        c[i] *= fact;
  }

#undef WA
#undef CC
#undef CH
#undef PMC

  public:
    template<typename T> NOINLINE void forward(T c[], T0 fct)
      { pass_all<false>(c, fct); }

    template<typename T> NOINLINE void backward(T c[], T0 fct)
      { pass_all<true>(c, fct); }

  private:
    NOINLINE void factorize()
      {
      nfct=0;
      size_t len=length;
      while ((len&3)==0)
        { add_factor(4); len>>=2; }
      if ((len&1)==0)
        {
        len>>=1;
        // factor 2 should be at the front of the factor list
        add_factor(2);
        swap(fct[0].fct, fct[nfct-1].fct);
        }
      size_t maxl=(size_t)(sqrt((double)len))+1;
      for (size_t divisor=3; (len>1)&&(divisor<maxl); divisor+=2)
        if ((len%divisor)==0)
          {
          while ((len%divisor)==0)
            {
            add_factor(divisor);
            len/=divisor;
            }
          maxl=size_t(sqrt((double)len))+1;
          }
      if (len>1) add_factor(len);
      }

    NOINLINE size_t twsize() const
      {
      size_t twsize=0, l1=1;
      for (size_t k=0; k<nfct; ++k)
        {
        size_t ip=fct[k].fct, ido= length/(l1*ip);
        twsize+=(ip-1)*(ido-1);
        if (ip>11)
          twsize+=ip;
        l1*=ip;
        }
      return twsize;
      }

    NOINLINE void comp_twiddle()
      {
      sincos_2pibyn twid(length, false);
      size_t l1=1;
      size_t memofs=0;
      for (size_t k=0; k<nfct; ++k)
        {
        size_t ip=fct[k].fct, ido=length/(l1*ip);
        fct[k].tw=mem.data()+memofs;
        memofs+=(ip-1)*(ido-1);
        for (size_t j=1; j<ip; ++j)
          for (size_t i=1; i<ido; ++i)
            {
            fct[k].tw[(j-1)*(ido-1)+i-1].r = twid[2*j*l1*i];
            fct[k].tw[(j-1)*(ido-1)+i-1].i = twid[2*j*l1*i+1];
            }
        if (ip>11)
          {
          fct[k].tws=mem.data()+memofs;
          memofs+=ip;
          for (size_t j=0; j<ip; ++j)
            {
            fct[k].tws[j].r = twid[2*j*l1*ido];
            fct[k].tws[j].i = twid[2*j*l1*ido+1];
            }
          }
        l1*=ip;
        }
      }

  public:
    NOINLINE cfftp(size_t length_)
      : length(length_), nfct(0)
      {
      if (length==0) throw runtime_error("zero length FFT requested");
      if (length==1) return;
      factorize();
      mem.resize(twsize());
      comp_twiddle();
      }
  };

//
// real-valued FFTPACK transforms
//

template<typename T0> class rfftp
  {
  private:
    struct fctdata
      {
      size_t fct;
      T0 *tw, *tws;
      };

    size_t length, nfct;
    arr<T0> mem;
    fctdata fct[NFCT];

    void add_factor(size_t factor)
      {
      if (nfct>=NFCT) throw runtime_error("too many prime factors");
      fct[nfct++].fct = factor;
      }

#define WA(x,i) wa[(i)+(x)*(ido-1)]
#define PM(a,b,c,d) { a=c+d; b=c-d; }
/* (a+ib) = conj(c+id) * (e+if) */
#define MULPM(a,b,c,d,e,f) { a=c*e+d*f; b=c*f-d*e; }

#define CC(a,b,c) cc[(a)+ido*((b)+l1*(c))]
#define CH(a,b,c) ch[(a)+ido*((b)+cdim*(c))]

template<typename T> NOINLINE void radf2 (size_t ido, size_t l1, const T * restrict cc,
  T * restrict ch, const T0 * restrict wa)
  {
  constexpr size_t cdim=2;

  for (size_t k=0; k<l1; k++)
    PM (CH(0,0,k),CH(ido-1,1,k),CC(0,k,0),CC(0,k,1))
  if ((ido&1)==0)
    for (size_t k=0; k<l1; k++)
      {
      CH(    0,1,k) = -CC(ido-1,k,1);
      CH(ido-1,0,k) =  CC(ido-1,k,0);
      }
  if (ido<=2) return;
  for (size_t k=0; k<l1; k++)
    for (size_t i=2; i<ido; i+=2)
      {
      size_t ic=ido-i;
      T tr2, ti2;
      MULPM (tr2,ti2,WA(0,i-2),WA(0,i-1),CC(i-1,k,1),CC(i,k,1))
      PM (CH(i-1,0,k),CH(ic-1,1,k),CC(i-1,k,0),tr2)
      PM (CH(i  ,0,k),CH(ic  ,1,k),ti2,CC(i  ,k,0))
      }
  }

template<typename T> NOINLINE void radf3(size_t ido, size_t l1, const T * restrict cc,
  T * restrict ch, const T0 * restrict wa)
  {
  constexpr size_t cdim=3;
  constexpr T0 taur=-0.5, taui=0.86602540378443864676;

  for (size_t k=0; k<l1; k++)
    {
    T cr2=CC(0,k,1)+CC(0,k,2);
    CH(0,0,k) = CC(0,k,0)+cr2;
    CH(0,2,k) = taui*(CC(0,k,2)-CC(0,k,1));
    CH(ido-1,1,k) = CC(0,k,0)+taur*cr2;
    }
  if (ido==1) return;
  for (size_t k=0; k<l1; k++)
    for (size_t i=2; i<ido; i+=2)
      {
      size_t ic=ido-i;
      T di2, di3, dr2, dr3;
      MULPM (dr2,di2,WA(0,i-2),WA(0,i-1),CC(i-1,k,1),CC(i,k,1)) // d2=conj(WA0)*CC1
      MULPM (dr3,di3,WA(1,i-2),WA(1,i-1),CC(i-1,k,2),CC(i,k,2)) // d3=conj(WA1)*CC2
      T cr2=dr2+dr3; // c add
      T ci2=di2+di3;
      CH(i-1,0,k) = CC(i-1,k,0)+cr2; // c add
      CH(i  ,0,k) = CC(i  ,k,0)+ci2;
      T tr2 = CC(i-1,k,0)+taur*cr2; // c add
      T ti2 = CC(i  ,k,0)+taur*ci2;
      T tr3 = taui*(di2-di3);  // t3 = taui*i*(d3-d2)?
      T ti3 = taui*(dr3-dr2);
      PM(CH(i-1,2,k),CH(ic-1,1,k),tr2,tr3) // PM(i) = t2+t3
      PM(CH(i  ,2,k),CH(ic  ,1,k),ti3,ti2) // PM(ic) = conj(t2-t3)
      }
  }

template<typename T> NOINLINE void radf4(size_t ido, size_t l1, const T * restrict cc,
  T * restrict ch, const T0 * restrict wa)
  {
  constexpr size_t cdim=4;
  constexpr T0 hsqt2=0.70710678118654752440;

  for (size_t k=0; k<l1; k++)
    {
    T tr1,tr2;
    PM (tr1,CH(0,2,k),CC(0,k,3),CC(0,k,1))
    PM (tr2,CH(ido-1,1,k),CC(0,k,0),CC(0,k,2))
    PM (CH(0,0,k),CH(ido-1,3,k),tr2,tr1)
    }
  if ((ido&1)==0)
    for (size_t k=0; k<l1; k++)
      {
      T ti1=-hsqt2*(CC(ido-1,k,1)+CC(ido-1,k,3));
      T tr1= hsqt2*(CC(ido-1,k,1)-CC(ido-1,k,3));
      PM (CH(ido-1,0,k),CH(ido-1,2,k),CC(ido-1,k,0),tr1)
      PM (CH(    0,3,k),CH(    0,1,k),ti1,CC(ido-1,k,2))
      }
  if (ido<=2) return;
  for (size_t k=0; k<l1; k++)
    for (size_t i=2; i<ido; i+=2)
      {
      size_t ic=ido-i;
      T ci2, ci3, ci4, cr2, cr3, cr4, ti1, ti2, ti3, ti4, tr1, tr2, tr3, tr4;
      MULPM(cr2,ci2,WA(0,i-2),WA(0,i-1),CC(i-1,k,1),CC(i,k,1))
      MULPM(cr3,ci3,WA(1,i-2),WA(1,i-1),CC(i-1,k,2),CC(i,k,2))
      MULPM(cr4,ci4,WA(2,i-2),WA(2,i-1),CC(i-1,k,3),CC(i,k,3))
      PM(tr1,tr4,cr4,cr2)
      PM(ti1,ti4,ci2,ci4)
      PM(tr2,tr3,CC(i-1,k,0),cr3)
      PM(ti2,ti3,CC(i  ,k,0),ci3)
      PM(CH(i-1,0,k),CH(ic-1,3,k),tr2,tr1)
      PM(CH(i  ,0,k),CH(ic  ,3,k),ti1,ti2)
      PM(CH(i-1,2,k),CH(ic-1,1,k),tr3,ti4)
      PM(CH(i  ,2,k),CH(ic  ,1,k),tr4,ti3)
      }
  }

template<typename T> NOINLINE void radf5(size_t ido, size_t l1, const T * restrict cc,
  T * restrict ch, const T0 * restrict wa)
  {
  constexpr size_t cdim=5;
  constexpr T0 tr11= 0.3090169943749474241, ti11=0.95105651629515357212,
               tr12=-0.8090169943749474241, ti12=0.58778525229247312917;

  for (size_t k=0; k<l1; k++)
    {
    T cr2, cr3, ci4, ci5;
    PM (cr2,ci5,CC(0,k,4),CC(0,k,1))
    PM (cr3,ci4,CC(0,k,3),CC(0,k,2))
    CH(0,0,k)=CC(0,k,0)+cr2+cr3;
    CH(ido-1,1,k)=CC(0,k,0)+tr11*cr2+tr12*cr3;
    CH(0,2,k)=ti11*ci5+ti12*ci4;
    CH(ido-1,3,k)=CC(0,k,0)+tr12*cr2+tr11*cr3;
    CH(0,4,k)=ti12*ci5-ti11*ci4;
    }
  if (ido==1) return;
  for (size_t k=0; k<l1;++k)
    for (size_t i=2; i<ido; i+=2)
      {
      T ci2, di2, ci4, ci5, di3, di4, di5, ci3, cr2, cr3, dr2, dr3,
        dr4, dr5, cr5, cr4, ti2, ti3, ti5, ti4, tr2, tr3, tr4, tr5;
      size_t ic=ido-i;
      MULPM (dr2,di2,WA(0,i-2),WA(0,i-1),CC(i-1,k,1),CC(i,k,1))
      MULPM (dr3,di3,WA(1,i-2),WA(1,i-1),CC(i-1,k,2),CC(i,k,2))
      MULPM (dr4,di4,WA(2,i-2),WA(2,i-1),CC(i-1,k,3),CC(i,k,3))
      MULPM (dr5,di5,WA(3,i-2),WA(3,i-1),CC(i-1,k,4),CC(i,k,4))
      PM(cr2,ci5,dr5,dr2)
      PM(ci2,cr5,di2,di5)
      PM(cr3,ci4,dr4,dr3)
      PM(ci3,cr4,di3,di4)
      CH(i-1,0,k)=CC(i-1,k,0)+cr2+cr3;
      CH(i  ,0,k)=CC(i  ,k,0)+ci2+ci3;
      tr2=CC(i-1,k,0)+tr11*cr2+tr12*cr3;
      ti2=CC(i  ,k,0)+tr11*ci2+tr12*ci3;
      tr3=CC(i-1,k,0)+tr12*cr2+tr11*cr3;
      ti3=CC(i  ,k,0)+tr12*ci2+tr11*ci3;
      MULPM(tr5,tr4,cr5,cr4,ti11,ti12)
      MULPM(ti5,ti4,ci5,ci4,ti11,ti12)
      PM(CH(i-1,2,k),CH(ic-1,1,k),tr2,tr5)
      PM(CH(i  ,2,k),CH(ic  ,1,k),ti5,ti2)
      PM(CH(i-1,4,k),CH(ic-1,3,k),tr3,tr4)
      PM(CH(i  ,4,k),CH(ic  ,3,k),ti4,ti3)
      }
  }

#undef CC
#undef CH
#define C1(a,b,c) cc[(a)+ido*((b)+l1*(c))]
#define C2(a,b) cc[(a)+idl1*(b)]
#define CH2(a,b) ch[(a)+idl1*(b)]
#define CC(a,b,c) cc[(a)+ido*((b)+cdim*(c))]
#define CH(a,b,c) ch[(a)+ido*((b)+l1*(c))]
template<typename T> NOINLINE void radfg(size_t ido, size_t ip, size_t l1,
  T * restrict cc, T * restrict ch, const T0 * restrict wa,
  const T0 * restrict csarr)
  {
  const size_t cdim=ip;
  size_t ipph=(ip+1)/2;
  size_t idl1 = ido*l1;

  if (ido>1)
    {
    for (size_t j=1, jc=ip-1; j<ipph; ++j,--jc)              // 114
      {
      size_t is=(j-1)*(ido-1),
             is2=(jc-1)*(ido-1);
      for (size_t k=0; k<l1; ++k)                            // 113
        {
        size_t idij=is;
        size_t idij2=is2;
        for (size_t i=1; i<=ido-2; i+=2)                      // 112
          {
          T t1=C1(i,k,j ), t2=C1(i+1,k,j ),
                 t3=C1(i,k,jc), t4=C1(i+1,k,jc);
          T x1=wa[idij]*t1 + wa[idij+1]*t2,
                 x2=wa[idij]*t2 - wa[idij+1]*t1,
                 x3=wa[idij2]*t3 + wa[idij2+1]*t4,
                 x4=wa[idij2]*t4 - wa[idij2+1]*t3;
          C1(i  ,k,j ) = x1+x3;
          C1(i  ,k,jc) = x2-x4;
          C1(i+1,k,j ) = x2+x4;
          C1(i+1,k,jc) = x3-x1;
          idij+=2;
          idij2+=2;
          }
        }
      }
    }

  for (size_t j=1, jc=ip-1; j<ipph; ++j,--jc)                // 123
    for (size_t k=0; k<l1; ++k)                              // 122
      {
      T t1=C1(0,k,j), t2=C1(0,k,jc);
      C1(0,k,j ) = t1+t2;
      C1(0,k,jc) = t2-t1;
      }

//everything in C
//memset(ch,0,ip*l1*ido*sizeof(double));

  for (size_t l=1,lc=ip-1; l<ipph; ++l,--lc)                 // 127
    {
    for (size_t ik=0; ik<idl1; ++ik)                         // 124
      {
      CH2(ik,l ) = C2(ik,0)+csarr[2*l]*C2(ik,1)+csarr[4*l]*C2(ik,2);
      CH2(ik,lc) = csarr[2*l+1]*C2(ik,ip-1)+csarr[4*l+1]*C2(ik,ip-2);
      }
    size_t iang = 2*l;
    size_t j=3, jc=ip-3;
    for (; j<ipph-3; j+=4,jc-=4)              // 126
      {
      iang+=l; if (iang>=ip) iang-=ip;
      T0 ar1=csarr[2*iang], ai1=csarr[2*iang+1];
      iang+=l; if (iang>=ip) iang-=ip;
      T0 ar2=csarr[2*iang], ai2=csarr[2*iang+1];
      iang+=l; if (iang>=ip) iang-=ip;
      T0 ar3=csarr[2*iang], ai3=csarr[2*iang+1];
      iang+=l; if (iang>=ip) iang-=ip;
      T0 ar4=csarr[2*iang], ai4=csarr[2*iang+1];
      for (size_t ik=0; ik<idl1; ++ik)                       // 125
        {
        CH2(ik,l ) += ar1*C2(ik,j )+ar2*C2(ik,j +1)
                     +ar3*C2(ik,j +2)+ar4*C2(ik,j +3);
        CH2(ik,lc) += ai1*C2(ik,jc)+ai2*C2(ik,jc-1)
                     +ai3*C2(ik,jc-2)+ai4*C2(ik,jc-3);
        }
      }
    for (; j<ipph-1; j+=2,jc-=2)              // 126
      {
      iang+=l; if (iang>=ip) iang-=ip;
      T0 ar1=csarr[2*iang], ai1=csarr[2*iang+1];
      iang+=l; if (iang>=ip) iang-=ip;
      T0 ar2=csarr[2*iang], ai2=csarr[2*iang+1];
      for (size_t ik=0; ik<idl1; ++ik)                       // 125
        {
        CH2(ik,l ) += ar1*C2(ik,j )+ar2*C2(ik,j +1);
        CH2(ik,lc) += ai1*C2(ik,jc)+ai2*C2(ik,jc-1);
        }
      }
    for (; j<ipph; ++j,--jc)              // 126
      {
      iang+=l; if (iang>=ip) iang-=ip;
      T0 ar=csarr[2*iang], ai=csarr[2*iang+1];
      for (size_t ik=0; ik<idl1; ++ik)                       // 125
        {
        CH2(ik,l ) += ar*C2(ik,j );
        CH2(ik,lc) += ai*C2(ik,jc);
        }
      }
    }
  for (size_t ik=0; ik<idl1; ++ik)                         // 101
    CH2(ik,0) = C2(ik,0);
  for (size_t j=1; j<ipph; ++j)                              // 129
    for (size_t ik=0; ik<idl1; ++ik)                         // 128
      CH2(ik,0) += C2(ik,j);

// everything in CH at this point!
//memset(cc,0,ip*l1*ido*sizeof(double));

  for (size_t k=0; k<l1; ++k)                                // 131
    for (size_t i=0; i<ido; ++i)                             // 130
      CC(i,0,k) = CH(i,k,0);

  for (size_t j=1, jc=ip-1; j<ipph; ++j,--jc)                // 137
    {
    size_t j2=2*j-1;
    for (size_t k=0; k<l1; ++k)                              // 136
      {
      CC(ido-1,j2,k) = CH(0,k,j);
      CC(0,j2+1,k) = CH(0,k,jc);
      }
    }

  if (ido==1) return;

  for (size_t j=1, jc=ip-1; j<ipph; ++j,--jc)                // 140
    {
    size_t j2=2*j-1;
    for(size_t k=0; k<l1; ++k)                               // 139
      for(size_t i=1, ic=ido-i-2; i<=ido-2; i+=2, ic-=2)      // 138
        {
        CC(i   ,j2+1,k) = CH(i  ,k,j )+CH(i  ,k,jc);
        CC(ic  ,j2  ,k) = CH(i  ,k,j )-CH(i  ,k,jc);
        CC(i+1 ,j2+1,k) = CH(i+1,k,j )+CH(i+1,k,jc);
        CC(ic+1,j2  ,k) = CH(i+1,k,jc)-CH(i+1,k,j );
        }
    }
  }
#undef C1
#undef C2
#undef CH2

#undef CH
#undef CC
#define CH(a,b,c) ch[(a)+ido*((b)+l1*(c))]
#define CC(a,b,c) cc[(a)+ido*((b)+cdim*(c))]

template<typename T> NOINLINE void radb2(size_t ido, size_t l1, const T * restrict cc,
  T * restrict ch, const T0 * restrict wa)
  {
  constexpr size_t cdim=2;

  for (size_t k=0; k<l1; k++)
    PM (CH(0,k,0),CH(0,k,1),CC(0,0,k),CC(ido-1,1,k))
  if ((ido&1)==0)
    for (size_t k=0; k<l1; k++)
      {
      CH(ido-1,k,0) = 2.*CC(ido-1,0,k);
      CH(ido-1,k,1) =-2.*CC(0    ,1,k);
      }
  if (ido<=2) return;
  for (size_t k=0; k<l1;++k)
    for (size_t i=2; i<ido; i+=2)
      {
      size_t ic=ido-i;
      T ti2, tr2;
      PM (CH(i-1,k,0),tr2,CC(i-1,0,k),CC(ic-1,1,k))
      PM (ti2,CH(i  ,k,0),CC(i  ,0,k),CC(ic  ,1,k))
      MULPM (CH(i,k,1),CH(i-1,k,1),WA(0,i-2),WA(0,i-1),ti2,tr2)
      }
  }

template<typename T> NOINLINE void radb3(size_t ido, size_t l1, const T * restrict cc,
  T * restrict ch, const T0 * restrict wa)
  {
  constexpr size_t cdim=3;
  constexpr T0 taur=-0.5, taui=0.86602540378443864676;

  for (size_t k=0; k<l1; k++)
    {
    T tr2=2.*CC(ido-1,1,k);
    T cr2=CC(0,0,k)+taur*tr2;
    CH(0,k,0)=CC(0,0,k)+tr2;
    T ci3=T0(2.)*taui*CC(0,2,k);
    PM (CH(0,k,2),CH(0,k,1),cr2,ci3);
    }
  if (ido==1) return;
  for (size_t k=0; k<l1; k++)
    for (size_t i=2; i<ido; i+=2)
      {
      size_t ic=ido-i;
      T tr2=CC(i-1,2,k)+CC(ic-1,1,k); // t2=CC(I) + conj(CC(ic))
      T ti2=CC(i  ,2,k)-CC(ic  ,1,k);
      T cr2=CC(i-1,0,k)+taur*tr2;     // c2=CC +taur*t2
      T ci2=CC(i  ,0,k)+taur*ti2;
      CH(i-1,k,0)=CC(i-1,0,k)+tr2;         // CH=CC+t2
      CH(i  ,k,0)=CC(i  ,0,k)+ti2;
      T cr3=taui*(CC(i-1,2,k)-CC(ic-1,1,k));// c3=taui*(CC(i)-conj(CC(ic)))
      T ci3=taui*(CC(i  ,2,k)+CC(ic  ,1,k));
      T di2, di3, dr2, dr3;
      PM(dr3,dr2,cr2,ci3) // d2= (cr2-ci3, ci2+cr3) = c2+i*c3
      PM(di2,di3,ci2,cr3) // d3= (cr2+ci3, ci2-cr3) = c2-i*c3
      MULPM(CH(i,k,1),CH(i-1,k,1),WA(0,i-2),WA(0,i-1),di2,dr2) // ch = WA*d2
      MULPM(CH(i,k,2),CH(i-1,k,2),WA(1,i-2),WA(1,i-1),di3,dr3)
      }
  }

template<typename T> NOINLINE void radb4(size_t ido, size_t l1, const T * restrict cc,
  T * restrict ch, const T0 * restrict wa)
  {
  constexpr size_t cdim=4;
  constexpr T0 sqrt2=1.41421356237309504880;

  for (size_t k=0; k<l1; k++)
    {
    T tr1, tr2;
    PM (tr2,tr1,CC(0,0,k),CC(ido-1,3,k))
    T tr3=2.*CC(ido-1,1,k);
    T tr4=2.*CC(0,2,k);
    PM (CH(0,k,0),CH(0,k,2),tr2,tr3)
    PM (CH(0,k,3),CH(0,k,1),tr1,tr4)
    }
  if ((ido&1)==0)
    for (size_t k=0; k<l1; k++)
      {
      T tr1,tr2,ti1,ti2;
      PM (ti1,ti2,CC(0    ,3,k),CC(0    ,1,k))
      PM (tr2,tr1,CC(ido-1,0,k),CC(ido-1,2,k))
      CH(ido-1,k,0)=tr2+tr2;
      CH(ido-1,k,1)=sqrt2*(tr1-ti1);
      CH(ido-1,k,2)=ti2+ti2;
      CH(ido-1,k,3)=-sqrt2*(tr1+ti1);
      }
  if (ido<=2) return;
  for (size_t k=0; k<l1;++k)
    for (size_t i=2; i<ido; i+=2)
      {
      T ci2, ci3, ci4, cr2, cr3, cr4, ti1, ti2, ti3, ti4, tr1, tr2, tr3, tr4;
      size_t ic=ido-i;
      PM (tr2,tr1,CC(i-1,0,k),CC(ic-1,3,k))
      PM (ti1,ti2,CC(i  ,0,k),CC(ic  ,3,k))
      PM (tr4,ti3,CC(i  ,2,k),CC(ic  ,1,k))
      PM (tr3,ti4,CC(i-1,2,k),CC(ic-1,1,k))
      PM (CH(i-1,k,0),cr3,tr2,tr3)
      PM (CH(i  ,k,0),ci3,ti2,ti3)
      PM (cr4,cr2,tr1,tr4)
      PM (ci2,ci4,ti1,ti4)
      MULPM (CH(i,k,1),CH(i-1,k,1),WA(0,i-2),WA(0,i-1),ci2,cr2)
      MULPM (CH(i,k,2),CH(i-1,k,2),WA(1,i-2),WA(1,i-1),ci3,cr3)
      MULPM (CH(i,k,3),CH(i-1,k,3),WA(2,i-2),WA(2,i-1),ci4,cr4)
      }
  }

template<typename T>NOINLINE void radb5(size_t ido, size_t l1, const T * restrict cc,
  T * restrict ch, const T0 * restrict wa)
  {
  constexpr size_t cdim=5;
  constexpr T0 tr11= 0.3090169943749474241, ti11=0.95105651629515357212,
               tr12=-0.8090169943749474241, ti12=0.58778525229247312917;

  for (size_t k=0; k<l1; k++)
    {
    T ti5=CC(0,2,k)+CC(0,2,k);
    T ti4=CC(0,4,k)+CC(0,4,k);
    T tr2=CC(ido-1,1,k)+CC(ido-1,1,k);
    T tr3=CC(ido-1,3,k)+CC(ido-1,3,k);
    CH(0,k,0)=CC(0,0,k)+tr2+tr3;
    T cr2=CC(0,0,k)+tr11*tr2+tr12*tr3;
    T cr3=CC(0,0,k)+tr12*tr2+tr11*tr3;
    T ci4, ci5;
    MULPM(ci5,ci4,ti5,ti4,ti11,ti12)
    PM(CH(0,k,4),CH(0,k,1),cr2,ci5)
    PM(CH(0,k,3),CH(0,k,2),cr3,ci4)
    }
  if (ido==1) return;
  for (size_t k=0; k<l1;++k)
    for (size_t i=2; i<ido; i+=2)
      {
      size_t ic=ido-i;
      T tr2, tr3, tr4, tr5, ti2, ti3, ti4, ti5;
      PM(tr2,tr5,CC(i-1,2,k),CC(ic-1,1,k))
      PM(ti5,ti2,CC(i  ,2,k),CC(ic  ,1,k))
      PM(tr3,tr4,CC(i-1,4,k),CC(ic-1,3,k))
      PM(ti4,ti3,CC(i  ,4,k),CC(ic  ,3,k))
      CH(i-1,k,0)=CC(i-1,0,k)+tr2+tr3;
      CH(i  ,k,0)=CC(i  ,0,k)+ti2+ti3;
      T cr2=CC(i-1,0,k)+tr11*tr2+tr12*tr3;
      T ci2=CC(i  ,0,k)+tr11*ti2+tr12*ti3;
      T cr3=CC(i-1,0,k)+tr12*tr2+tr11*tr3;
      T ci3=CC(i  ,0,k)+tr12*ti2+tr11*ti3;
      T ci4, ci5, cr5, cr4;
      MULPM(cr5,cr4,tr5,tr4,ti11,ti12)
      MULPM(ci5,ci4,ti5,ti4,ti11,ti12)
      T dr2, dr3, dr4, dr5, di2, di3, di4, di5;
      PM(dr4,dr3,cr3,ci4)
      PM(di3,di4,ci3,cr4)
      PM(dr5,dr2,cr2,ci5)
      PM(di2,di5,ci2,cr5)
      MULPM(CH(i,k,1),CH(i-1,k,1),WA(0,i-2),WA(0,i-1),di2,dr2)
      MULPM(CH(i,k,2),CH(i-1,k,2),WA(1,i-2),WA(1,i-1),di3,dr3)
      MULPM(CH(i,k,3),CH(i-1,k,3),WA(2,i-2),WA(2,i-1),di4,dr4)
      MULPM(CH(i,k,4),CH(i-1,k,4),WA(3,i-2),WA(3,i-1),di5,dr5)
      }
  }

#undef CC
#undef CH
#define CC(a,b,c) cc[(a)+ido*((b)+cdim*(c))]
#define CH(a,b,c) ch[(a)+ido*((b)+l1*(c))]
#define C1(a,b,c) cc[(a)+ido*((b)+l1*(c))]
#define C2(a,b) cc[(a)+idl1*(b)]
#define CH2(a,b) ch[(a)+idl1*(b)]

template<typename T> NOINLINE void radbg(size_t ido, size_t ip, size_t l1,
  T * restrict cc, T * restrict ch, const T0 * restrict wa,
  const T0 * restrict csarr)
  {
  const size_t cdim=ip;
  size_t ipph=(ip+1)/ 2;
  size_t idl1 = ido*l1;

  for (size_t k=0; k<l1; ++k)        // 102
    for (size_t i=0; i<ido; ++i)     // 101
      CH(i,k,0) = CC(i,0,k);
  for (size_t j=1, jc=ip-1; j<ipph; ++j, --jc)   // 108
    {
    size_t j2=2*j-1;
    for (size_t k=0; k<l1; ++k)
      {
      CH(0,k,j ) = 2*CC(ido-1,j2,k);
      CH(0,k,jc) = 2*CC(0,j2+1,k);
      }
    }

  if (ido!=1)
    {
    for (size_t j=1, jc=ip-1; j<ipph; ++j,--jc)   // 111
      {
      size_t j2=2*j-1;
      for (size_t k=0; k<l1; ++k)
        for (size_t i=1, ic=ido-i-2; i<=ido-2; i+=2, ic-=2)      // 109
          {
          CH(i  ,k,j ) = CC(i  ,j2+1,k)+CC(ic  ,j2,k);
          CH(i  ,k,jc) = CC(i  ,j2+1,k)-CC(ic  ,j2,k);
          CH(i+1,k,j ) = CC(i+1,j2+1,k)-CC(ic+1,j2,k);
          CH(i+1,k,jc) = CC(i+1,j2+1,k)+CC(ic+1,j2,k);
          }
      }
    }
  for (size_t l=1,lc=ip-1; l<ipph; ++l,--lc)
    {
    for (size_t ik=0; ik<idl1; ++ik)
      {
      C2(ik,l ) = CH2(ik,0)+csarr[2*l]*CH2(ik,1)+csarr[4*l]*CH2(ik,2);
      C2(ik,lc) = csarr[2*l+1]*CH2(ik,ip-1)+csarr[4*l+1]*CH2(ik,ip-2);
      }
    size_t iang=2*l;
    size_t j=3,jc=ip-3;
    for(; j<ipph-3; j+=4,jc-=4)
      {
      iang+=l; if(iang>ip) iang-=ip;
      T0 ar1=csarr[2*iang], ai1=csarr[2*iang+1];
      iang+=l; if(iang>ip) iang-=ip;
      T0 ar2=csarr[2*iang], ai2=csarr[2*iang+1];
      iang+=l; if(iang>ip) iang-=ip;
      T0 ar3=csarr[2*iang], ai3=csarr[2*iang+1];
      iang+=l; if(iang>ip) iang-=ip;
      T0 ar4=csarr[2*iang], ai4=csarr[2*iang+1];
      for (size_t ik=0; ik<idl1; ++ik)
        {
        C2(ik,l ) += ar1*CH2(ik,j )+ar2*CH2(ik,j +1)
                    +ar3*CH2(ik,j +2)+ar4*CH2(ik,j +3);
        C2(ik,lc) += ai1*CH2(ik,jc)+ai2*CH2(ik,jc-1)
                    +ai3*CH2(ik,jc-2)+ai4*CH2(ik,jc-3);
        }
      }
    for(; j<ipph-1; j+=2,jc-=2)
      {
      iang+=l; if(iang>ip) iang-=ip;
      T0 ar1=csarr[2*iang], ai1=csarr[2*iang+1];
      iang+=l; if(iang>ip) iang-=ip;
      T0 ar2=csarr[2*iang], ai2=csarr[2*iang+1];
      for (size_t ik=0; ik<idl1; ++ik)
        {
        C2(ik,l ) += ar1*CH2(ik,j )+ar2*CH2(ik,j +1);
        C2(ik,lc) += ai1*CH2(ik,jc)+ai2*CH2(ik,jc-1);
        }
      }
    for(; j<ipph; ++j,--jc)
      {
      iang+=l; if(iang>ip) iang-=ip;
      T0 war=csarr[2*iang], wai=csarr[2*iang+1];
      for (size_t ik=0; ik<idl1; ++ik)
        {
        C2(ik,l ) += war*CH2(ik,j );
        C2(ik,lc) += wai*CH2(ik,jc);
        }
      }
    }
  for (size_t j=1; j<ipph; ++j)
    for (size_t ik=0; ik<idl1; ++ik)
      CH2(ik,0) += CH2(ik,j);
  for (size_t j=1, jc=ip-1; j<ipph; ++j,--jc)   // 124
    for (size_t k=0; k<l1; ++k)
      {
      CH(0,k,j ) = C1(0,k,j)-C1(0,k,jc);
      CH(0,k,jc) = C1(0,k,j)+C1(0,k,jc);
      }

  if (ido==1) return;

  for (size_t j=1, jc=ip-1; j<ipph; ++j, --jc)  // 127
    for (size_t k=0; k<l1; ++k)
      for (size_t i=1; i<=ido-2; i+=2)
        {
        CH(i  ,k,j ) = C1(i  ,k,j)-C1(i+1,k,jc);
        CH(i  ,k,jc) = C1(i  ,k,j)+C1(i+1,k,jc);
        CH(i+1,k,j ) = C1(i+1,k,j)+C1(i  ,k,jc);
        CH(i+1,k,jc) = C1(i+1,k,j)-C1(i  ,k,jc);
        }

// All in CH

  for (size_t j=1; j<ip; ++j)
    {
    size_t is = (j-1)*(ido-1);
    for (size_t k=0; k<l1; ++k)
      {
      size_t idij = is;
      for (size_t i=1; i<=ido-2; i+=2)
        {
        T t1=CH(i,k,j), t2=CH(i+1,k,j);
        CH(i  ,k,j) = wa[idij]*t1-wa[idij+1]*t2;
        CH(i+1,k,j) = wa[idij]*t2+wa[idij+1]*t1;
        idij+=2;
        }
      }
    }
  }
#undef C1
#undef C2
#undef CH2

#undef CC
#undef CH
#undef PM
#undef MULPM
#undef WA

template<typename T> void copy_and_norm(T *c, T *p1, size_t n, T0 fct)
  {
  if (p1!=c)
    {
    if (fct!=1.)
      for (size_t i=0; i<n; ++i)
        c[i] = fct*p1[i];
    else
      memcpy (c,p1,n*sizeof(T));
    }
  else
    if (fct!=1.)
      for (size_t i=0; i<n; ++i)
        c[i] *= fct;
  }

  public:

template<typename T> void forward(T c[], T0 fact)
  {
  if (length==1) { c[0]*=fact; return; }
  size_t n=length;
  size_t l1=n, nf=nfct;
  arr<T> ch(n);
  T *p1=c, *p2=ch.data();

  for(size_t k1=0; k1<nf;++k1)
    {
    size_t k=nf-k1-1;
    size_t ip=fct[k].fct;
    size_t ido=n / l1;
    l1 /= ip;
    if(ip==4)
      radf4(ido, l1, p1, p2, fct[k].tw);
    else if(ip==2)
      radf2(ido, l1, p1, p2, fct[k].tw);
    else if(ip==3)
      radf3(ido, l1, p1, p2, fct[k].tw);
    else if(ip==5)
      radf5(ido, l1, p1, p2, fct[k].tw);
    else
      {
      radfg(ido, ip, l1, p1, p2, fct[k].tw, fct[k].tws);
      swap (p1,p2);
      }
    swap (p1,p2);
    }
  copy_and_norm(c,p1,n,fact);
  }

template<typename T> void backward(T c[], T0 fact)
  {
  if (length==1) { c[0]*=fact; return; }
  size_t n=length;
  size_t l1=1, nf=nfct;
  arr<T> ch(n);
  T *p1=c, *p2=ch.data();

  for(size_t k=0; k<nf; k++)
    {
    size_t ip = fct[k].fct,
           ido= n/(ip*l1);
    if(ip==4)
      radb4(ido, l1, p1, p2, fct[k].tw);
    else if(ip==2)
      radb2(ido, l1, p1, p2, fct[k].tw);
    else if(ip==3)
      radb3(ido, l1, p1, p2, fct[k].tw);
    else if(ip==5)
      radb5(ido, l1, p1, p2, fct[k].tw);
    else
      radbg(ido, ip, l1, p1, p2, fct[k].tw, fct[k].tws);
    swap (p1,p2);
    l1*=ip;
    }
  copy_and_norm(c,p1,n,fact);
  }

  private:

void factorize()
  {
  size_t len=length;
  nfct=0;
  while ((len%4)==0)
    { add_factor(4); len>>=2; }
  if ((len%2)==0)
    {
    len>>=1;
    // factor 2 should be at the front of the factor list
    add_factor(2);
    swap(fct[0].fct, fct[nfct-1].fct);
    }
  size_t maxl=(size_t)(sqrt((double)len))+1;
  for (size_t divisor=3; (len>1)&&(divisor<maxl); divisor+=2)
    if ((len%divisor)==0)
      {
      while ((len%divisor)==0)
        {
        add_factor(divisor);
        len/=divisor;
        }
      maxl=(size_t)(sqrt((double)len))+1;
      }
  if (len>1) add_factor(len);
  }

size_t twsize() const
  {
  size_t twsz=0, l1=1;
  for (size_t k=0; k<nfct; ++k)
    {
    size_t ip=fct[k].fct, ido=length/(l1*ip);
    twsz+=(ip-1)*(ido-1);
    if (ip>5) twsz+=2*ip;
    l1*=ip;
    }
  return twsz;
  }

NOINLINE void comp_twiddle()
  {
  sincos_2pibyn twid(length, true);
  size_t l1=1;
  T0 *ptr=mem.data();
  for (size_t k=0; k<nfct; ++k)
    {
    size_t ip=fct[k].fct, ido=length/(l1*ip);
    if (k<nfct-1) // last factor doesn't need twiddles
      {
      fct[k].tw=ptr; ptr+=(ip-1)*(ido-1);
      for (size_t j=1; j<ip; ++j)
        for (size_t i=1; i<=(ido-1)/2; ++i)
          {
          fct[k].tw[(j-1)*(ido-1)+2*i-2] = twid[2*j*l1*i];
          fct[k].tw[(j-1)*(ido-1)+2*i-1] = twid[2*j*l1*i+1];
          }
      }
    if (ip>5) // special factors required by *g functions
      {
      fct[k].tws=ptr; ptr+=2*ip;
      fct[k].tws[0] = 1.;
      fct[k].tws[1] = 0.;
      for (size_t i=1; i<=(ip>>1); ++i)
        {
        fct[k].tws[2*i  ] = twid[2*i*(length/ip)];
        fct[k].tws[2*i+1] = twid[2*i*(length/ip)+1];
        fct[k].tws[2*(ip-i)  ] = twid[2*i*(length/ip)];
        fct[k].tws[2*(ip-i)+1] = -twid[2*i*(length/ip)+1];
        }
      }
    l1*=ip;
    }
  }

  public:

NOINLINE rfftp(size_t length_)
  : length(length_)
  {
  if (length==0) throw runtime_error("zero_sized FFT");
  nfct=0;
  if (length==1) return;
  factorize();
  mem.resize(twsize());
  comp_twiddle();
  }

};

//
// complex Bluestein transforms
//

template<typename T0> class fftblue
  {
  private:
    size_t n, n2;
    cfftp<T0> plan;
    arr<T0> mem;
    T0 *bk, *bkf;

    template<bool bwd, typename T> NOINLINE
    void fft(T c[], T0 fct)
      {
      constexpr T0 isign = bwd ? 1 : -1;
      arr<T> akf(2*n2);

      /* initialize a_k and FFT it */
      for (size_t m=0; m<2*n; m+=2)
        {
        akf[m]   = c[m]*bk[m]   -isign*c[m+1]*bk[m+1];
        akf[m+1] = isign*c[m]*bk[m+1] + c[m+1]*bk[m];
        }
      for (size_t m=2*n; m<2*n2; ++m)
        akf[m]=0.*c[0];

      plan.forward ((cmplx<T> *)akf.data(),1.);

      /* do the convolution */
      for (size_t m=0; m<2*n2; m+=2)
        {
        T im = -isign*akf[m]*bkf[m+1] + akf[m+1]*bkf[m];
        akf[m  ]  =  akf[m]*bkf[m]   + isign*akf[m+1]*bkf[m+1];
        akf[m+1]  = im;
        }

      /* inverse FFT */
      plan.backward ((cmplx<T> *)akf.data(),1.);

      /* multiply by b_k */
      for (size_t m=0; m<2*n; m+=2)
        {
        c[m]   = fct*(bk[m]  *akf[m] - isign*bk[m+1]*akf[m+1]);
        c[m+1] = fct*(isign*bk[m+1]*akf[m] + bk[m]  *akf[m+1]);
        }
      }

  public:
    NOINLINE fftblue(size_t length)
      : n(length), n2(good_size(n*2-1)), plan(n2), mem(2*(n+n2)),
        bk(mem.data()), bkf(mem.data()+2*n)
      {
      /* initialize b_k */
      sincos_2pibyn tmp(2*n, false);
      bk[0] = 1;
      bk[1] = 0;

      size_t coeff=0;
      for (size_t m=1; m<n; ++m)
        {
        coeff+=2*m-1;
        if (coeff>=2*n) coeff-=2*n;
        bk[2*m  ] = tmp[2*coeff  ];
        bk[2*m+1] = tmp[2*coeff+1];
        }

      /* initialize the zero-padded, Fourier transformed b_k. Add normalisation. */
      T0 xn2 = 1./n2;
      bkf[0] = bk[0]*xn2;
      bkf[1] = bk[1]*xn2;
      for (size_t m=2; m<2*n; m+=2)
        {
        bkf[m]   = bkf[2*n2-m]   = bk[m]   *xn2;
        bkf[m+1] = bkf[2*n2-m+1] = bk[m+1] *xn2;
        }
      for (size_t m=2*n;m<=(2*n2-2*n+1);++m)
        bkf[m]=0.;
      plan.forward((cmplx<T0> *)bkf,1.);
      }

    template<typename T> void backward(T c[], T0 fct)
      { fft<true>(c,fct); }

    template<typename T> void forward(T c[], T0 fct)
      { fft<false>(c,fct); }

    template<typename T> void backward_r(T c[], T0 fct)
      {
      arr<T> tmp(2*n);
      tmp[0]=c[0];
      tmp[1]=c[0]*0.;
      memcpy (tmp.data()+2,c+1, (n-1)*sizeof(T));
      if ((n&1)==0) tmp[n+1]=c[0]*0.;
      for (size_t m=2; m<n; m+=2)
        {
        tmp[2*n-m]=tmp[m];
        tmp[2*n-m+1]=-tmp[m+1];
        }
      fft<true>(tmp.data(),fct);
      for (size_t m=0; m<n; ++m)
        c[m] = tmp[2*m];
      }

    template<typename T> void forward_r(T c[], T0 fct)
      {
      arr<T> tmp(2*n);
      for (size_t m=0; m<n; ++m)
        {
        tmp[2*m] = c[m];
        tmp[2*m+1] = c[m]*0.;
        }
      fft<false>(tmp.data(),fct);
      c[0] = tmp[0];
      memcpy (c+1, tmp.data()+2, (n-1)*sizeof(T));
      }
    };

//
// flexible (FFTPACK/Bluestein) complex 1D transform
//

template<typename T0> class pocketfft_c
  {
  private:
    unique_ptr<cfftp<T0>> packplan;
    unique_ptr<fftblue<T0>> blueplan;
    size_t len;

  public:
    NOINLINE pocketfft_c(size_t length)
      : len(length)
      {
      if (length==0) throw runtime_error("zero-length FFT requested");
      if ((length<50) || (largest_prime_factor(length)<=sqrt(length)))
        {
        packplan=unique_ptr<cfftp<T0>>(new cfftp<T0>(length));
        return;
        }
      double comp1 = cost_guess(length);
      double comp2 = 2*cost_guess(good_size(2*length-1));
      comp2*=1.5; /* fudge factor that appears to give good overall performance */
      if (comp2<comp1) // use Bluestein
        blueplan=unique_ptr<fftblue<T0>>(new fftblue<T0>(length));
      else
        packplan=unique_ptr<cfftp<T0>>(new cfftp<T0>(length));
      }

    template<typename T> void backward(T c[], T0 fct)
      {
      packplan ? packplan->backward((cmplx<T> *)c,fct)
               : blueplan->backward(c,fct);
      }

    template<typename T> void forward(T c[], T0 fct)
      {
      packplan ? packplan->forward((cmplx<T> *)c,fct)
               : blueplan->forward(c,fct);
      }

    size_t length() const { return len; }
  };

//
// flexible (FFTPACK/Bluestein) real-valued 1D transform
//

template<typename T0> class pocketfft_r
  {
  private:
    unique_ptr<rfftp<T0>> packplan;
    unique_ptr<fftblue<T0>> blueplan;
    size_t len;

  public:
    NOINLINE pocketfft_r(size_t length)
      : len(length)
      {
      if (length==0) throw runtime_error("zero-length FFT requested");
      if ((length<50) || (largest_prime_factor(length)<=sqrt(length)))
        {
        packplan=unique_ptr<rfftp<T0>>(new rfftp<T0>(length));
        return;
        }
      double comp1 = 0.5*cost_guess(length);
      double comp2 = 2*cost_guess(good_size(2*length-1));
      comp2*=1.5; /* fudge factor that appears to give good overall performance */
      if (comp2<comp1) // use Bluestein
        blueplan=unique_ptr<fftblue<T0>>(new fftblue<T0>(length));
      else
        packplan=unique_ptr<rfftp<T0>>(new rfftp<T0>(length));
      }

    template<typename T> void backward(T c[], T0 fct)
      {
      packplan ? packplan->backward(c,fct)
               : blueplan->backward_r(c,fct);
      }

    template<typename T> void forward(T c[], T0 fct)
      {
      packplan ? packplan->forward(c,fct)
               : blueplan->forward_r(c,fct);
      }

    size_t length() const { return len; }
  };

//
// multi-D infrastructure
//

using shape_t = vector<size_t>;
using stride_t = vector<int64_t>;

struct diminfo
  { size_t n; int64_t s; };
class multiarr
  {
  private:
    vector<diminfo> dim;

  public:
    multiarr (const shape_t &n, const stride_t &s)
      {
      dim.reserve(n.size());
      for (size_t i=0; i<n.size(); ++i)
        dim.push_back({n[i], s[i]});
      }
    size_t ndim() const { return dim.size(); }
    size_t size(size_t i) const { return dim[i].n; }
    int64_t stride(size_t i) const { return dim[i].s; }
  };

class multi_iter
  {
  public:
    vector<diminfo> dim;
    shape_t pos;
    size_t ofs_, len;
    int64_t str;
    int64_t rem;
    bool done_;

  public:
    multi_iter(const multiarr &arr, size_t idim)
      : pos(arr.ndim()-1, 0), ofs_(0), len(arr.size(idim)),
        str(arr.stride(idim)), rem(1), done_(false)
      {
      dim.reserve(arr.ndim()-1);
      for (size_t i=0; i<arr.ndim(); ++i)
        if (i!=idim)
          {
          dim.push_back({arr.size(i), arr.stride(i)});
          done_ = done_ || (arr.size(i)==0);
          rem *= arr.size(i);
          }
      }
    void advance()
      {
      if (--rem<=0) {done_=true; return; }
      for (int i=pos.size()-1; i>=0; --i)
        {
        ++pos[i];
        ofs_ += dim[i].s;
        if (pos[i] < dim[i].n)
          return;
        pos[i] = 0;
        ofs_ -= dim[i].n*dim[i].s;
        }
      done_ = true;
      }
    bool done() const { return done_; }
    size_t offset() const { return ofs_; }
    size_t length() const { return len; }
    int64_t stride() const { return str; }
    int64_t remaining() const { return rem; }
  };


#if (defined(__AVX512F__))
#include <x86intrin.h>
#define HAVE_VECSUPPORT
template<typename T> struct VTYPE{};
template<> struct VTYPE<double>
  { using type = __m512d; };
template<> struct VTYPE<float>
  { using type = __m512; };
#elif (defined(__AVX__))
#include <x86intrin.h>
#define HAVE_VECSUPPORT
template<typename T> struct VTYPE{};
template<> struct VTYPE<double>
  { using type = __m256d; };
template<> struct VTYPE<float>
  { using type = __m256; };
#elif (defined(__SSE2__))
#include <x86intrin.h>
#define HAVE_VECSUPPORT
template<typename T> struct VTYPE{};
template<> struct VTYPE<double>
  { using type = __m128d; };
template<> struct VTYPE<float>
  { using type = __m128; };
#endif

template<typename T> arr<char> alloc_tmp(const shape_t &shape,
  size_t tmpsize, size_t elemsize)
  {
#ifdef HAVE_VECSUPPORT
  using vtype = typename VTYPE<T>::type;
  constexpr int vlen = sizeof(vtype)/sizeof(T);
  return arr<char>(tmpsize*elemsize*vlen);
#endif
  return arr<char>(tmpsize*elemsize);
  }
template<typename T> arr<char> alloc_tmp(const shape_t &shape,
  const shape_t &axes, size_t elemsize)
  {
  size_t fullsize=1;
  size_t ndim = shape.size();
  for (size_t i=0; i<ndim; ++i)
    fullsize*=shape[i];
  size_t tmpsize=0;
  for (size_t i=0; i<axes.size(); ++i)
    tmpsize = max(tmpsize, shape[axes[i]]);

  return alloc_tmp<T>(shape, tmpsize, elemsize);
  }

template<typename T> void pocketfft_general_c(const shape_t &shape,
  const stride_t &stride_in, const stride_t &stride_out,
  const shape_t &axes, bool forward, const cmplx<T> *data_in,
  cmplx<T> *data_out, T fct)
  {
  auto storage = alloc_tmp<T>(shape, axes, sizeof(cmplx<T>));
#ifdef HAVE_VECSUPPORT
  using vtype = typename VTYPE<T>::type;
  auto tdatav = (cmplx<vtype> *)storage.data();
  constexpr int vlen = sizeof(vtype)/sizeof(T);
#endif
  auto tdata = (cmplx<T> *)storage.data();

  multiarr a_in(shape, stride_in), a_out(shape, stride_out);
  unique_ptr<pocketfft_c<T>> plan;

  for (size_t iax=0; iax<axes.size(); ++iax)
    {
    int axis = axes[iax];
    multi_iter it_in(a_in, axis), it_out(a_out, axis);
    if ((!plan) || (it_in.length()!=plan->length()))
      plan.reset(new pocketfft_c<T>(it_in.length()));
#ifdef HAVE_VECSUPPORT
    while (it_in.remaining()>=vlen)
      {
      size_t p_i[vlen];
      for (size_t i=0; i<vlen; ++i)
        { p_i[i] = it_in.offset(); it_in.advance(); }
      size_t p_o[vlen];
      for (size_t i=0; i<vlen; ++i)
        { p_o[i] = it_out.offset(); it_out.advance(); }
      for (size_t i=0; i<it_in.length(); ++i)
        for (size_t j=0; j<vlen; ++j)
          {
          tdatav[i].r[j] = data_in[p_i[j]+i*it_in.stride()].r;
          tdatav[i].i[j] = data_in[p_i[j]+i*it_in.stride()].i;
          }
      forward ? plan->forward((vtype *)tdatav, fct)
              : plan->backward((vtype *)tdatav, fct);
      for (size_t i=0; i<it_out.length(); ++i)
        for (size_t j=0; j<vlen; ++j)
          data_out[p_o[j]+i*it_out.stride()].Set(tdatav[i].r[j],tdatav[i].i[j]);
      }
#endif
    while (it_in.remaining()>0)
      {
      for (size_t i=0; i<it_in.length(); ++i)
        tdata[i] = data_in[it_in.offset() + i*it_in.stride()];
      forward ? plan->forward((T *)tdata, fct)
              : plan->backward((T *)tdata, fct);
      for (size_t i=0; i<it_out.length(); ++i)
        data_out[it_out.offset()+i*it_out.stride()] = tdata[i];
      it_in.advance();
      it_out.advance();
      }
    // after the first dimension, take data from output array
    a_in = a_out;
    data_in = data_out;
    // factor has been applied, use 1 for remaining axes
    fct = T(1);
    }
  }

template<typename T> void pocketfft_general_hartley(const shape_t &shape,
  const stride_t &stride_in, const stride_t &stride_out,
  const shape_t &axes, const T *data_in, T *data_out, T fct)
  {
  auto storage = alloc_tmp<T>(shape, axes, sizeof(T));
#ifdef HAVE_VECSUPPORT
  using vtype = typename VTYPE<T>::type;
  auto tdatav = (vtype *)storage.data();
  constexpr int vlen = sizeof(vtype)/sizeof(T);
#endif
  auto tdata = (T *)storage.data();

  multiarr a_in(shape, stride_in), a_out(shape, stride_out);
  unique_ptr<pocketfft_r<T>> plan;

  for (size_t iax=0; iax<axes.size(); ++iax)
    {
    int axis = axes[iax];
    multi_iter it_in(a_in, axis), it_out(a_out, axis);
    if ((!plan) || (it_in.length()!=plan->length()))
      plan.reset(new pocketfft_r<T>(it_in.length()));
#ifdef HAVE_VECSUPPORT
    while (it_in.remaining()>=vlen)
      {
      size_t p_i[vlen];
      for (size_t i=0; i<vlen; ++i)
        { p_i[i] = it_in.offset(); it_in.advance(); }
      size_t p_o[vlen];
      for (size_t i=0; i<vlen; ++i)
        { p_o[i] = it_out.offset(); it_out.advance(); }
      for (size_t i=0; i<it_in.length(); ++i)
        for (size_t j=0; j<vlen; ++j)
          tdatav[i][j] = data_in[p_i[j]+i*it_in.stride()];
      plan->forward((vtype *)tdatav, fct);
      for (size_t j=0; j<vlen; ++j)
        data_out[p_o[j]] = tdatav[0][j];
      size_t i=1, i1=1, i2=it_out.length()-1;
      for (i=1; i<it_out.length()-1; i+=2, ++i1, --i2)
        for (size_t j=0; j<vlen; ++j)
          {
          data_out[p_o[j]+i1*it_out.stride()] = tdatav[i][j]+tdatav[i+1][j];
          data_out[p_o[j]+i2*it_out.stride()] = tdatav[i][j]-tdatav[i+1][j];
          }
      if (i<it_out.length())
        for (size_t j=0; j<vlen; ++j)
          data_out[p_o[j]+i1*it_out.stride()] = tdatav[i][j];
      }
#endif
    while (it_in.remaining()>0)
      {
      for (size_t i=0; i<it_in.length(); ++i)
        tdata[i] = data_in[it_in.offset()+i*it_in.stride()];
      plan->forward((T *)tdata, fct);
      // Hartley order
      data_out[it_out.offset()] = tdata[0];
      size_t i=1, i1=1, i2=it_out.length()-1;
      for (i=1; i<it_out.length()-1; i+=2, ++i1, --i2)
        {
        data_out[it_out.offset()+i1*it_out.stride()] = tdata[i]+tdata[i+1];
        data_out[it_out.offset()+i2*it_out.stride()] = tdata[i]-tdata[i+1];
        }
      if (i<it_out.length())
        data_out[it_out.offset()+i1*it_out.stride()] = tdata[i];
      it_in.advance();
      it_out.advance();
      }
    // after the first dimension, take data from output array
    a_in = a_out;
    data_in = data_out;
    // factor has been applied, use 1 for remaining axes
    fct = T(1);
    }
  }

template<typename T> void pocketfft_general_r2c(const shape_t &shape,
  const stride_t &stride_in, const stride_t &stride_out, size_t axis,
  const T *data_in, cmplx<T> *data_out, T fct)
  {
  auto storage = alloc_tmp<T>(shape, shape[axis], sizeof(T));
#ifdef HAVE_VECSUPPORT
  using vtype = typename VTYPE<T>::type;
  auto tdatav = (vtype *)storage.data();
  constexpr int vlen = sizeof(vtype)/sizeof(T);
#endif
  auto tdata = (T *)storage.data();

  multiarr a_in(shape, stride_in), a_out(shape, stride_out);
  pocketfft_r<T> plan(shape[axis]);
  multi_iter it_in(a_in, axis), it_out(a_out, axis);
  size_t len=shape[axis], s_i=it_in.stride(), s_o=it_out.stride();
#ifdef HAVE_VECSUPPORT
  while (it_in.remaining()>=vlen)
    {
    size_t p_i[vlen];
    for (size_t i=0; i<vlen; ++i)
      { p_i[i] = it_in.offset(); it_in.advance(); }
    size_t p_o[vlen];
    for (size_t i=0; i<vlen; ++i)
      { p_o[i] = it_out.offset(); it_out.advance(); }
    for (size_t i=0; i<it_in.length(); ++i)
      for (size_t j=0; j<vlen; ++j)
        tdatav[i][j] = data_in[p_i[j]+i*it_in.stride()];
    plan.forward((vtype *)tdatav, fct);
    for (size_t j=0; j<vlen; ++j)
      data_out[p_o[j]].Set(tdatav[0][j]);
    size_t i;
    for (i=1; i<len-1; i+=2)
      {
      size_t io = (i+1)/2;
      for (size_t j=0; j<vlen; ++j)
        data_out[p_o[j]+io*it_out.stride()].Set(tdatav[i][j], tdatav[i+1][j]);
      }
    if (i<len)
      {
      size_t io = (i+1)/2;
      for (size_t j=0; j<vlen; ++j)
        data_out[p_o[j]+io*it_out.stride()].Set(tdatav[i][j]);
      }
    }
#endif
  while (it_in.remaining()>0)
    {
    const T *d_i = data_in+it_in.offset();
    cmplx<T> *d_o = data_out+it_out.offset();
    for (size_t i=0; i<len; ++i)
      tdata[i] = d_i[i*s_i];
    plan.forward(tdata, fct);
    d_o[0].Set(tdata[0]);
    size_t i;
    for (i=1; i<len-1; i+=2)
      d_o[((i+1)/2)*s_o].Set(tdata[i], tdata[i+1]);
    if (i<len)
      d_o[((i+1)/2)*s_o].Set(tdata[i]);
    it_in.advance();
    it_out.advance();
    }
  }
template<typename T> void pocketfft_general_c2r(const shape_t &shape_out,
  const stride_t &stride_in, const stride_t &stride_out, size_t axis,
  const cmplx<T> *data_in, T *data_out, T fct)
  {
  auto storage = alloc_tmp<T>(shape_out, shape_out[axis], sizeof(T));
#ifdef HAVE_VECSUPPORT
  using vtype = typename VTYPE<T>::type;
  auto tdatav = (vtype *)storage.data();
  constexpr int vlen = sizeof(vtype)/sizeof(T);
#endif
  auto tdata = (T *)storage.data();

  multiarr a_in(shape_out, stride_in), a_out(shape_out, stride_out);
  pocketfft_r<T> plan(shape_out[axis]);
  multi_iter it_in(a_in, axis), it_out(a_out, axis);
  size_t len=shape_out[axis], s_i=it_in.stride(), s_o=it_out.stride();
#ifdef HAVE_VECSUPPORT
  while (it_in.remaining()>=vlen)
    {
    size_t p_i[vlen];
    for (size_t i=0; i<vlen; ++i)
      { p_i[i] = it_in.offset(); it_in.advance(); }
    size_t p_o[vlen];
    for (size_t i=0; i<vlen; ++i)
      { p_o[i] = it_out.offset(); it_out.advance(); }
    for (size_t j=0; j<vlen; ++j)
      tdatav[0][j]=data_in[p_i[j]].r;
    size_t i;
    for (i=1; i<len-1; i+=2)
      {
      size_t ii = (i+1)/2;
      for (size_t j=0; j<vlen; ++j)
        {
        tdatav[i][j] = data_in[p_i[j]+ii*s_i].r;
        tdatav[i+1][j] = data_in[p_i[j]+ii*s_i].i;
        }
      }
    if (i<len)
      {
      size_t ii = (i+1)/2;
      for (size_t j=0; j<vlen; ++j)
        tdatav[i][j] = data_in[p_i[j]+ii*s_i].r;
      }
    plan.backward(tdatav, fct);
    for (size_t i=0; i<len; ++i)
      for (size_t j=0; j<vlen; ++j)
        data_out[p_o[j]+i*s_o] = tdatav[i][j];
    }
#endif
  while (!it_in.done())
    {
    const cmplx<T> *d_i = data_in+it_in.offset();
    T *d_o = data_out+it_out.offset();
    tdata[0]=d_i[0].r;
    size_t i;
    for (i=1; i<len-1; i+=2)
      {
      size_t ii = (i+1)/2;
      tdata[i] = d_i[ii*s_i].r;
      tdata[i+1] = d_i[ii*s_i].i;
      }
    if (i<len)
      tdata[i] = d_i[((i+1)/2)*s_i].r;
    plan.backward(tdata, fct);
    for (size_t i=0; i<len; ++i)
      d_o[i*s_o] = tdata[i];
    it_in.advance();
    it_out.advance();
    }
  }

} // unnamed namespace

#include "pocketfft.h"

int pocketfft_complex(size_t ndim, const size_t *shape,
  const int64_t *stride_in, const int64_t *stride_out, size_t nax,
  const size_t *axes, int forward, const void *data_in,
  void *data_out, double fct, int dp)
  {
  try {
    if (nax>ndim) return 1;
    shape_t xshape(ndim), xaxes(nax);
    stride_t xstride_in(ndim), xstride_out(ndim);
    for (size_t i=0; i<ndim; ++i)
      {
      xshape[i] = shape[i];
      xstride_in[i] = stride_in[i];
      xstride_out[i] = stride_out[i];
      }
    for (size_t i=0; i<nax; ++i)
      xaxes[i] = axes[i];
    if (dp)
      pocketfft_general_c(xshape, xstride_in, xstride_out, xaxes, bool(forward),
        (const cmplx<double> *)data_in, (cmplx<double> *) data_out, fct);
    else
      pocketfft_general_c(xshape, xstride_in, xstride_out, xaxes, bool(forward),
        (const cmplx<float> *)data_in, (cmplx<float> *) data_out, float(fct));
    }
  catch(...) {
    return 1;
    }
  return 0;
  }