/* (C) 2007 Jean-Marc Valin, CSIRO
*/
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:
   
   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
   
   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
   
   - Neither the name of the Xiph.org Foundation nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.
   
   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "os_support.h"
#include "mdct.h"
#include <math.h>
#include "celt.h"
#include "pitch.h"
#include "kiss_fftr.h"
#include "bands.h"
#include "modes.h"
#include "entcode.h"
#include "quant_pitch.h"
#include "quant_bands.h"
#include "psy.h"
#include "rate.h"

#define MAX_PERIOD 1024

#ifndef M_PI
#define M_PI 3.14159263
#endif

struct CELTEncoder {
   const CELTMode *mode;
   int frame_size;
   int block_size;
   int nb_blocks;
   int overlap;
   int channels;
   int Fs;
   
   ec_byte_buffer buf;
   ec_enc         enc;

   float preemph;
   float *preemph_memE;
   float *preemph_memD;
   
   mdct_lookup mdct_lookup;
   kiss_fftr_cfg fft;
   struct PsyDecay psy;
   
   float *window;
   float *in_mem;
   float *mdct_overlap;
   float *out_mem;

   float *oldBandE;
};



CELTEncoder *celt_encoder_new(const CELTMode *mode)
{
   int i, N, B, C, N4;
   CELTEncoder *st;
   N = mode->mdctSize;
   B = mode->nbMdctBlocks;
   C = mode->nbChannels;
   st = celt_alloc(sizeof(CELTEncoder));
   
   st->mode = mode;
   st->frame_size = B*N;
   st->block_size = N;
   st->nb_blocks  = B;
   st->overlap = mode->overlap;
   st->Fs = 44100;

   N4 = (N-st->overlap)/2;
   ec_byte_writeinit(&st->buf);
   ec_enc_init(&st->enc,&st->buf);

   mdct_init(&st->mdct_lookup, 2*N);
   st->fft = kiss_fftr_alloc(MAX_PERIOD*C, 0, 0);
   psydecay_init(&st->psy, MAX_PERIOD*C/2, st->Fs);
   
   st->window = celt_alloc(2*N*sizeof(float));
   st->in_mem = celt_alloc(N*C*sizeof(float));
   st->mdct_overlap = celt_alloc(N*C*sizeof(float));
   st->out_mem = celt_alloc(MAX_PERIOD*C*sizeof(float));
   for (i=0;i<2*N;i++)
      st->window[i] = 0;
   for (i=0;i<st->overlap;i++)
      st->window[N4+i] = st->window[2*N-N4-i-1] 
            = sin(.5*M_PI* sin(.5*M_PI*(i+.5)/st->overlap) * sin(.5*M_PI*(i+.5)/st->overlap));
   for (i=0;i<2*N4;i++)
      st->window[N-N4+i] = 1;
   st->oldBandE = celt_alloc(C*mode->nbEBands*sizeof(float));

   st->preemph = 0.8;
   st->preemph_memE = celt_alloc(C*sizeof(float));;
   st->preemph_memD = celt_alloc(C*sizeof(float));;

   return st;
}

void celt_encoder_destroy(CELTEncoder *st)
{
   if (st == NULL)
   {
      celt_warning("NULL passed to celt_encoder_destroy");
      return;
   }
   ec_byte_writeclear(&st->buf);

   mdct_clear(&st->mdct_lookup);
   kiss_fft_free(st->fft);
   psydecay_clear(&st->psy);

   celt_free(st->window);
   celt_free(st->in_mem);
   celt_free(st->mdct_overlap);
   celt_free(st->out_mem);
   
   celt_free(st->oldBandE);
   
   celt_free(st->preemph_memE);
   celt_free(st->preemph_memD);
   
   celt_free(st);
}


static float compute_mdcts(mdct_lookup *mdct_lookup, float *window, float *in, float *out, int N, int B, int C)
{
   int i, c;
   float E = 1e-15;
   VARDECL(float *x);
   VARDECL(float *tmp);
   ALLOC(x, 2*N, float);
   ALLOC(tmp, N, float);
   for (c=0;c<C;c++)
   {
      for (i=0;i<B;i++)
      {
         int j;
         for (j=0;j<2*N;j++)
         {
            x[j] = window[j]*in[C*i*N+C*j+c];
            E += x[j]*x[j];
         }
         mdct_forward(mdct_lookup, x, tmp);
         /* Interleaving the sub-frames */
         for (j=0;j<N;j++)
            out[C*B*j+C*i+c] = tmp[j];
      }
   }
   return E;
}

static void compute_inv_mdcts(mdct_lookup *mdct_lookup, float *window, float *X, float *out_mem, float *mdct_overlap, int N, int overlap, int B, int C)
{
   int i, c, N4;
   VARDECL(float *x);
   VARDECL(float *tmp);
   ALLOC(x, 2*N, float);
   ALLOC(tmp, N, float);
   N4 = (N-overlap)/2;
   for (c=0;c<C;c++)
   {
      for (i=0;i<B;i++)
      {
         int j;
         /* De-interleaving the sub-frames */
         for (j=0;j<N;j++)
            tmp[j] = X[C*B*j+C*i+c];
         mdct_backward(mdct_lookup, tmp, x);
         for (j=0;j<2*N;j++)
            x[j] = window[j]*x[j];
         for (j=0;j<overlap;j++)
            out_mem[C*(MAX_PERIOD+(i-B)*N)+C*j+c] = x[N4+j]+mdct_overlap[C*j+c];
         for (j=0;j<2*N4;j++)
            out_mem[C*(MAX_PERIOD+(i-B)*N)+C*(j+overlap)+c] = x[j+N4+overlap];
         for (j=0;j<overlap;j++)
            mdct_overlap[C*j+c] = x[N+N4+j];
      }
   }
}

int celt_encode(CELTEncoder *st, celt_int16_t *pcm, unsigned char *compressed, int nbCompressedBytes)
{
   int i, c, N, B, C, N4;
   int has_pitch;
   int pitch_index;
   float curr_power, pitch_power;
   VARDECL(float *in);
   VARDECL(float *X);
   VARDECL(float *P);
   VARDECL(float *mask);
   VARDECL(float *bandE);
   VARDECL(float *gains);
   N = st->block_size;
   B = st->nb_blocks;
   C = st->mode->nbChannels;
   ALLOC(in, (B+1)*C*N, float);
   ALLOC(X, B*C*N, float);         /**< Interleaved signal MDCTs */
   ALLOC(P, B*C*N, float);         /**< Interleaved pitch MDCTs*/
   ALLOC(mask, B*C*N, float);      /**< Masking curve */
   ALLOC(bandE,st->mode->nbEBands*C, float);
   ALLOC(gains,st->mode->nbPBands, float);
   
   N4 = (N-st->overlap)/2;

   for (c=0;c<C;c++)
   {
      for (i=0;i<N4;i++)
         in[C*i+c] = 0;
      for (i=0;i<st->overlap;i++)
         in[C*(i+N4)+c] = st->in_mem[C*i+c];
      for (i=0;i<B*N;i++)
      {
         float tmp = pcm[C*i+c];
         in[C*(i+st->overlap+N4)+c] = tmp - st->preemph*st->preemph_memE[c];
         st->preemph_memE[c] = tmp;
      }
      for (i=N*(B+1)-N4;i<N*(B+1);i++)
         in[C*i+c] = 0;
      for (i=0;i<st->overlap;i++)
         st->in_mem[C*i+c] = in[C*(N*(B+1)-N4-st->overlap+i)+c];
   }
   /*for (i=0;i<(B+1)*C*N;i++) printf ("%f(%d) ", in[i], i); printf ("\n");*/
   /* Compute MDCTs */
   curr_power = compute_mdcts(&st->mdct_lookup, st->window, in, X, N, B, C);

#if 0 /* Mask disabled until it can be made to do something useful */
   compute_mdct_masking(X, mask, B*C*N, st->Fs);

   /* Invert and stretch the mask to length of X 
      For some reason, I get better results by using the sqrt instead,
      although there's no valid reason to. Must investigate further */
   for (i=0;i<B*C*N;i++)
      mask[i] = 1/(.1+mask[i]);
#else
   for (i=0;i<B*C*N;i++)
      mask[i] = 1;
#endif
   /* Pitch analysis */
   for (c=0;c<C;c++)
   {
      for (i=0;i<N;i++)
      {
         in[C*i+c] *= st->window[i];
         in[C*(B*N+i)+c] *= st->window[N+i];
      }
   }
   find_spectral_pitch(st->fft, &st->psy, in, st->out_mem, MAX_PERIOD, (B+1)*N, C, &pitch_index);
   
   /* Compute MDCTs of the pitch part */
   pitch_power = compute_mdcts(&st->mdct_lookup, st->window, st->out_mem+pitch_index*C, P, N, B, C);
   
   /*printf ("%f %f\n", curr_power, pitch_power);*/
   /*int j;
   for (j=0;j<B*N;j++)
      printf ("%f ", X[j]);
   for (j=0;j<B*N;j++)
      printf ("%f ", P[j]);
   printf ("\n");*/

   /* Band normalisation */
   compute_band_energies(st->mode, X, bandE);
   normalise_bands(st->mode, X, bandE);
   /*for (i=0;i<st->mode->nbEBands;i++)printf("%f ", bandE[i]);printf("\n");*/
   /*for (i=0;i<N*B*C;i++)printf("%f ", X[i]);printf("\n");*/

   quant_energy(st->mode, bandE, st->oldBandE, nbCompressedBytes*8/3, &st->enc);

   if (C==2)
   {
      stereo_mix(st->mode, X, bandE, 1);
   }

   /* Check if we can safely use the pitch (i.e. effective gain isn't too high) */
   if (curr_power + 1e5f < 10.f*pitch_power)
   {
      /* Normalise the pitch vector as well (discard the energies) */
      VARDECL(float *bandEp);
      ALLOC(bandEp, st->mode->nbEBands*st->mode->nbChannels, float);
      compute_band_energies(st->mode, P, bandEp);
      normalise_bands(st->mode, P, bandEp);

      if (C==2)
         stereo_mix(st->mode, P, bandE, 1);
      /* Simulates intensity stereo */
      /*for (i=30;i<N*B;i++)
         X[i*C+1] = P[i*C+1] = 0;*/

      /* Pitch prediction */
      compute_pitch_gain(st->mode, X, P, gains, bandE);
      has_pitch = quant_pitch(gains, st->mode->nbPBands, &st->enc);
      if (has_pitch)
         ec_enc_uint(&st->enc, pitch_index, MAX_PERIOD-(B+1)*N);
   } else {
      /* No pitch, so we just pretend we found a gain of zero */
      for (i=0;i<st->mode->nbPBands;i++)
         gains[i] = 0;
      ec_enc_uint(&st->enc, 0, 128);
      for (i=0;i<B*C*N;i++)
         P[i] = 0;
   }
   

   pitch_quant_bands(st->mode, X, P, gains);

   /*for (i=0;i<B*N;i++) printf("%f ",P[i]);printf("\n");*/
   /* Compute residual that we're going to encode */
   for (i=0;i<B*C*N;i++)
      X[i] -= P[i];

   /*float sum=0;
   for (i=0;i<B*N;i++)
      sum += X[i]*X[i];
   printf ("%f\n", sum);*/
   /* Residual quantisation */
   quant_bands(st->mode, X, P, mask, nbCompressedBytes*8, &st->enc);
   
   if (C==2)
      stereo_mix(st->mode, X, bandE, -1);

   renormalise_bands(st->mode, X);
   /* Synthesis */
   denormalise_bands(st->mode, X, bandE);


   CELT_MOVE(st->out_mem, st->out_mem+C*B*N, C*(MAX_PERIOD-B*N));

   compute_inv_mdcts(&st->mdct_lookup, st->window, X, st->out_mem, st->mdct_overlap, N, st->overlap, B, C);
   /* De-emphasis and put everything back at the right place in the synthesis history */
   for (c=0;c<C;c++)
   {
      for (i=0;i<B;i++)
      {
         int j;
         for (j=0;j<N;j++)
         {
            float tmp = st->out_mem[C*(MAX_PERIOD+(i-B)*N)+C*j+c] + st->preemph*st->preemph_memD[c];
            st->preemph_memD[c] = tmp;
            if (tmp > 32767) tmp = 32767;
            if (tmp < -32767) tmp = -32767;
            pcm[C*i*N+C*j+c] = (short)floor(.5+tmp);
         }
      }
   }
   
   if (ec_enc_tell(&st->enc, 0) < nbCompressedBytes*8 - 7)
      celt_warning_int ("many unused bits: ", nbCompressedBytes*8-ec_enc_tell(&st->enc, 0));
   /*printf ("%d\n", ec_enc_tell(&st->enc, 0)-8*nbCompressedBytes);*/
   /* Finishing the stream with a 0101... pattern so that the decoder can check is everything's right */
   {
      int val = 0;
      while (ec_enc_tell(&st->enc, 0) < nbCompressedBytes*8)
      {
         ec_enc_uint(&st->enc, val, 2);
         val = 1-val;
      }
   }
   ec_enc_done(&st->enc);
   {
      unsigned char *data;
      int nbBytes = ec_byte_bytes(&st->buf);
      if (nbBytes > nbCompressedBytes)
      {
         celt_warning_int ("got too many bytes:", nbBytes);
         return CELT_INTERNAL_ERROR;
      }
      /*printf ("%d\n", *nbBytes);*/
      data = ec_byte_get_buffer(&st->buf);
      for (i=0;i<nbBytes;i++)
         compressed[i] = data[i];
      for (;i<nbCompressedBytes;i++)
         compressed[i] = 0;
   }
   /* Reset the packing for the next encoding */
   ec_byte_reset(&st->buf);
   ec_enc_init(&st->enc,&st->buf);

   return nbCompressedBytes;
}


/****************************************************************************/
/*                                                                          */
/*                                DECODER                                   */
/*                                                                          */
/****************************************************************************/



struct CELTDecoder {
   const CELTMode *mode;
   int frame_size;
   int block_size;
   int nb_blocks;
   int overlap;

   ec_byte_buffer buf;
   ec_enc         enc;

   float preemph;
   float *preemph_memD;
   
   mdct_lookup mdct_lookup;
   
   float *window;
   float *mdct_overlap;
   float *out_mem;

   float *oldBandE;
   
   int last_pitch_index;
};

CELTDecoder *celt_decoder_new(const CELTMode *mode)
{
   int i, N, B, C, N4;
   CELTDecoder *st;
   N = mode->mdctSize;
   B = mode->nbMdctBlocks;
   C = mode->nbChannels;
   st = celt_alloc(sizeof(CELTDecoder));
   
   st->mode = mode;
   st->frame_size = B*N;
   st->block_size = N;
   st->nb_blocks  = B;
   st->overlap = mode->overlap;

   N4 = (N-st->overlap)/2;
   
   mdct_init(&st->mdct_lookup, 2*N);
   
   st->window = celt_alloc(2*N*sizeof(float));
   st->mdct_overlap = celt_alloc(N*C*sizeof(float));
   st->out_mem = celt_alloc(MAX_PERIOD*C*sizeof(float));

   for (i=0;i<2*N;i++)
      st->window[i] = 0;
   for (i=0;i<st->overlap;i++)
      st->window[N4+i] = st->window[2*N-N4-i-1] 
            = sin(.5*M_PI* sin(.5*M_PI*(i+.5)/st->overlap) * sin(.5*M_PI*(i+.5)/st->overlap));
   for (i=0;i<2*N4;i++)
      st->window[N-N4+i] = 1;
   
   st->oldBandE = celt_alloc(C*mode->nbEBands*sizeof(float));

   st->preemph = 0.8;
   st->preemph_memD = celt_alloc(C*sizeof(float));;

   st->last_pitch_index = 0;
   return st;
}

void celt_decoder_destroy(CELTDecoder *st)
{
   if (st == NULL)
   {
      celt_warning("NULL passed to celt_encoder_destroy");
      return;
   }

   mdct_clear(&st->mdct_lookup);

   celt_free(st->window);
   celt_free(st->mdct_overlap);
   celt_free(st->out_mem);
   
   celt_free(st->oldBandE);
   
   celt_free(st->preemph_memD);

   celt_free(st);
}

static void celt_decode_lost(CELTDecoder *st, short *pcm)
{
   int i, c, N, B, C;
   int pitch_index;
   VARDECL(float *X);
   N = st->block_size;
   B = st->nb_blocks;
   C = st->mode->nbChannels;
   ALLOC(X,C*B*N, float);         /**< Interleaved signal MDCTs */
   
   pitch_index = st->last_pitch_index;
   
   /* Use the pitch MDCT as the "guessed" signal */
   compute_mdcts(&st->mdct_lookup, st->window, st->out_mem+pitch_index*C, X, N, B, C);

   CELT_MOVE(st->out_mem, st->out_mem+C*B*N, C*(MAX_PERIOD-B*N));
   /* Compute inverse MDCTs */
   compute_inv_mdcts(&st->mdct_lookup, st->window, X, st->out_mem, st->mdct_overlap, N, st->overlap, B, C);

   for (c=0;c<C;c++)
   {
      for (i=0;i<B;i++)
      {
         int j;
         for (j=0;j<N;j++)
         {
            float tmp = st->out_mem[C*(MAX_PERIOD+(i-B)*N)+C*j+c] + st->preemph*st->preemph_memD[c];
            st->preemph_memD[c] = tmp;
            if (tmp > 32767) tmp = 32767;
            if (tmp < -32767) tmp = -32767;
            pcm[C*i*N+C*j+c] = (short)floor(.5+tmp);
         }
      }
   }
}

int celt_decode(CELTDecoder *st, unsigned char *data, int len, celt_int16_t *pcm)
{
   int i, c, N, B, C;
   int has_pitch;
   int pitch_index;
   ec_dec dec;
   ec_byte_buffer buf;
   VARDECL(float *X);
   VARDECL(float *P);
   VARDECL(float *bandE);
   VARDECL(float *gains);
   N = st->block_size;
   B = st->nb_blocks;
   C = st->mode->nbChannels;
   
   ALLOC(X, C*B*N, float);         /**< Interleaved signal MDCTs */
   ALLOC(P, C*B*N, float);         /**< Interleaved pitch MDCTs*/
   ALLOC(bandE, st->mode->nbEBands*C, float);
   ALLOC(gains, st->mode->nbPBands, float);
   
   if (data == NULL)
   {
      celt_decode_lost(st, pcm);
      return 0;
   }
   
   ec_byte_readinit(&buf,data,len);
   ec_dec_init(&dec,&buf);
   
   /* Get band energies */
   unquant_energy(st->mode, bandE, st->oldBandE, len*8/3, &dec);
   
   /* Get the pitch gains */
   has_pitch = unquant_pitch(gains, st->mode->nbPBands, &dec);
   
   /* Get the pitch index */
   if (has_pitch)
   {
      pitch_index = ec_dec_uint(&dec, MAX_PERIOD-(B+1)*N);
      st->last_pitch_index = pitch_index;
   } else {
      /* FIXME: We could be more intelligent here and just not compute the MDCT */
      pitch_index = 0;
   }
   
   /* Pitch MDCT */
   compute_mdcts(&st->mdct_lookup, st->window, st->out_mem+pitch_index*C, P, N, B, C);

   {
      VARDECL(float *bandEp);
      ALLOC(bandEp, st->mode->nbEBands*C, float);
      compute_band_energies(st->mode, P, bandEp);
      normalise_bands(st->mode, P, bandEp);
   }

   if (C==2)
      stereo_mix(st->mode, P, bandE, 1);

   /* Apply pitch gains */
   pitch_quant_bands(st->mode, X, P, gains);

   /* Decode fixed codebook and merge with pitch */
   unquant_bands(st->mode, X, P, len*8, &dec);

   if (C==2)
      stereo_mix(st->mode, X, bandE, -1);

   renormalise_bands(st->mode, X);
   
   /* Synthesis */
   denormalise_bands(st->mode, X, bandE);


   CELT_MOVE(st->out_mem, st->out_mem+C*B*N, C*(MAX_PERIOD-B*N));
   /* Compute inverse MDCTs */
   compute_inv_mdcts(&st->mdct_lookup, st->window, X, st->out_mem, st->mdct_overlap, N, st->overlap, B, C);

   for (c=0;c<C;c++)
   {
      for (i=0;i<B;i++)
      {
         int j;
         for (j=0;j<N;j++)
         {
            float tmp = st->out_mem[C*(MAX_PERIOD+(i-B)*N)+C*j+c] + st->preemph*st->preemph_memD[c];
            st->preemph_memD[c] = tmp;
            if (tmp > 32767) tmp = 32767;
            if (tmp < -32767) tmp = -32767;
            pcm[C*i*N+C*j+c] = (short)floor(.5+tmp);
         }
      }
   }

   {
      int val = 0;
      while (ec_dec_tell(&dec, 0) < len*8)
      {
         if (ec_dec_uint(&dec, 2) != val)
         {
            celt_warning("decode error");
            return CELT_CORRUPTED_DATA;
         }
         val = 1-val;
      }
   }

   return 0;
   /*printf ("\n");*/
}

