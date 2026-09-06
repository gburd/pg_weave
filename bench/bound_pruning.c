/*-------------------------------------------------------------------------
 *
 * bound_pruning.c
 *		Does the vector-channel block bound actually prune?
 *
 * The decision-relevant question for doc/specs/FUSED_TOPK.md is NOT how tight
 * the bound is as a multiple of a block's own maximum score.  It is: once theta
 * has been set by the true top-k, what fraction of blocks can be skipped
 * without scoring a single lane?  That fraction is one minus the score()-call
 * ratio, which is the gate in FUSED_TOPK sect. 8 -- and the gate that decides
 * whether the whole fused-scorer design is worth building.
 *
 * This harness answers it for three bound formulations and two warp orderings.
 * Results are recorded in bench/RESULTS_BOUND_PRUNING.md.  Run:
 *
 *		gcc -O2 -I include -o /tmp/bound_pruning bench/bound_pruning.c \
 *			src/vector/quantize.c src/vector/pack.c -lm
 *		/tmp/bound_pruning 1     # coherent warp order
 *		/tmp/bound_pruning 0     # random warp order
 *
 * It also asserts soundness (channel.h contract C2) on every block of every
 * query and exits non-zero on the first violation, so it doubles as a large
 * randomized correctness test of the bound.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  bench/bound_pruning.c
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "weave/quantize.h"
static unsigned long long s[4]={0x9e3779b97f4a7c15ULL,0xbf58476d1ce4e5b9ULL,0x94d049bb133111ebULL,7};
static unsigned long long r64(void){unsigned long long r=((s[1]*5)<<7|(s[1]*5)>>57)*9,t=s[1]<<17;
 s[2]^=s[0];s[3]^=s[1];s[1]^=s[2];s[0]^=s[3];s[2]^=t;s[3]=(s[3]<<45)|(s[3]>>19);return r;}
static double u(void){return (double)(r64()>>11)*(1.0/9007199254740992.0);}
static double nrm(void){double a=u();if(a<1e-300)a=1e-300;return sqrt(-2*log(a))*cos(2*M_PI*u());}
static int cmpd(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return x<y?1:(x>y?-1:0);}

#define NBLK 256
#define LANES 32
#define N (NBLK*LANES)

int main(int argc,char**argv){
 int dim=256,bits=4,K=10,coherent=(argc>1?atoi(argv[1]):1);
 WeaveQuantizer q; weave_quantizer_init(&q,dim,bits,NULL,malloc,free);
 float *recs=malloc((size_t)4*dim*N), *v=malloc(4*dim), *qy=malloc(4*dim), *cen=malloc((size_t)4*dim*NBLK);
 unsigned char*code=malloc(q.codebytes);
 double *smax=calloc(NBLK,8),*mrec=calloc(NBLK,8),*rad=calloc(NBLK,8);
 float *clusterdir=malloc((size_t)4*dim*NBLK);
 /* Build the corpus.  coherent=1: each 32-lane block is one tight cluster,
  * which is what ordering the warp by the graph's k-means partition gives you.
  * coherent=0: random assignment, the pathological case. */
 for(int b=0;b<NBLK;b++) for(int j=0;j<dim;j++) clusterdir[(size_t)b*dim+j]=(float)nrm();
 for(int i=0;i<N;i++){
   int b=i/LANES; float n,sc;
   if(coherent){ float*c=clusterdir+(size_t)b*dim; for(int j=0;j<dim;j++) v[j]=(float)(c[j]+0.35*nrm()); }
   else for(int j=0;j<dim;j++) v[j]=(float)nrm();
   weave_encode(&q,v,code,&n,&sc); weave_decode(&q,code,sc,recs+(size_t)i*dim);
   double rn=0; for(int j=0;j<dim;j++){double t=recs[(size_t)i*dim+j];rn+=t*t;} rn=sqrt(rn);
   if(sc>smax[b])smax[b]=sc; if(rn>mrec[b])mrec[b]=rn;
 }
 for(int b=0;b<NBLK;b++){
   for(int j=0;j<dim;j++){double a=0;for(int i=0;i<LANES;i++)a+=recs[(size_t)(b*LANES+i)*dim+j];cen[(size_t)b*dim+j]=(float)(a/LANES);}
   for(int i=0;i<LANES;i++){double d=0;for(int j=0;j<dim;j++){double t=(double)recs[(size_t)(b*LANES+i)*dim+j]-cen[(size_t)b*dim+j];d+=t*t;}
     d=sqrt(d); if(d>rad[b])rad[b]=d;}
 }
 double tot_lut=0,tot_cs=0,tot_cen=0,tot_min=0; int nq=200;
 for(int t=0;t<nq;t++){
   /* realistic query: a perturbation of a real corpus vector, so true
    * neighbours exist and theta is meaningful */
   int anchor=(int)(r64()%N);
   for(int j=0;j<dim;j++) qy[j]=(float)(recs[(size_t)anchor*dim+j]+0.30*nrm());
   WeaveQueryLut lut; weave_query_lut_build(&lut,&q,qy,malloc);
   double qn=0; for(int j=0;j<dim;j++) qn+=(double)qy[j]*qy[j]; qn=sqrt(qn);
   double *sc=malloc(8*N);
   for(int i=0;i<N;i++){double ip=0;for(int j=0;j<dim;j++)ip+=(double)qy[j]*recs[(size_t)i*dim+j];sc[i]=ip;}
   double *cp=malloc(8*N); memcpy(cp,sc,8*N); qsort(cp,N,8,cmpd);
   double theta=cp[K-1];
   int p_lut=0,p_cs=0,p_cen=0,p_min=0;
   for(int b=0;b<NBLK;b++){
     double b1=smax[b]*lut.lutbound, b2=mrec[b]*qn;
     double ipc=0; for(int j=0;j<dim;j++) ipc+=(double)qy[j]*cen[(size_t)b*dim+j];
     double b3=ipc+qn*rad[b];
     double bm=b1; if(b2<bm)bm=b2; if(b3<bm)bm=b3;
     if(b1<=theta)p_lut++; if(b2<=theta)p_cs++; if(b3<=theta)p_cen++; if(bm<=theta)p_min++;
     /* soundness: the min bound must dominate every lane in the block */
     for(int i=0;i<LANES;i++) if(sc[b*LANES+i] > bm+1e-6*fabs(bm)){
        printf("BOUND VIOLATION blk=%d lane=%d score=%g bound=%g\n",b,i,sc[b*LANES+i],bm); exit(1);}
   }
   tot_lut+=(double)p_lut/NBLK; tot_cs+=(double)p_cs/NBLK; tot_cen+=(double)p_cen/NBLK; tot_min+=(double)p_min/NBLK;
   free(sc);free(cp);free(lut._alloc);
 }
 printf("%s blocks, dim=%d bits=%d k=%d, %d queries, %d blocks x %d lanes\n",
   coherent?"COHERENT (warp ordered by cluster)":"RANDOM (unordered warp)",dim,bits,K,nq,NBLK,LANES);
 printf("  blocks pruned by LUT bound      : %5.1f%%\n",100*tot_lut/nq);
 printf("  blocks pruned by Cauchy-Schwarz : %5.1f%%\n",100*tot_cs/nq);
 printf("  blocks pruned by centroid+radius: %5.1f%%\n",100*tot_cen/nq);
 printf("  blocks pruned by min of three   : %5.1f%%  -> score() call ratio %.3f\n",
   100*tot_min/nq, 1.0-tot_min/nq);
 return 0;}
