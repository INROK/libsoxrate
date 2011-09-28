/* Effect: change sample rate     Copyright (c) 2008 robs@users.sourceforge.net
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* Inspired by, and builds upon some of the ideas presented in:
 * `The Quest For The Perfect Resampler' by Laurent De Soras;
 * http://ldesoras.free.fr/doc/articles/resampler-en.pdf */

#include <string.h>
#include <math.h>
#include <assert.h>
#include <stdint.h>
#include "soxint.h"
#include "fft4g.h"
#include "fifo.h"

#define  raw_coef_t double
#define  sample_t   double
#define  coef(coef_p, interp_order, fir_len, phase_num, coef_interp_num, fir_coef_num) coef_p[(fir_len) * ((interp_order) + 1) * (phase_num) + ((interp_order) + 1) * (fir_coef_num) + (interp_order - coef_interp_num)]

static sample_t * prepare_coefs(raw_coef_t const * coefs, int num_coefs,
    int num_phases, int interp_order, int multiplier)
{
  int i, j, length = num_coefs * num_phases;
  sample_t * result = lsx_malloc(length * (interp_order + 1) * sizeof(*result));
  double fm1 = coefs[0], f1 = 0, f2 = 0;

  for (i = num_coefs - 1; i >= 0; --i)
    for (j = num_phases - 1; j >= 0; --j) {
      double f0 = fm1, b = 0, c = 0, d = 0; /* = 0 to kill compiler warning */
      int pos = i * num_phases + j - 1;
      fm1 = (pos > 0 ? coefs[pos - 1] : 0) * multiplier;
      switch (interp_order) {
        case 1: b = f1 - f0; break;
        case 2: b = f1 - (.5 * (f2+f0) - f1) - f0; c = .5 * (f2+f0) - f1; break;
        case 3: c=.5*(f1+fm1)-f0;d=(1/6.)*(f2-f1+fm1-f0-4*c);b=f1-f0-d-c; break;
        default: if (interp_order) assert(0);
      }
      #define coef_coef(x) \
        coef(result, interp_order, num_coefs, j, x, num_coefs - 1 - i)
      coef_coef(0) = f0;
      if (interp_order > 0) coef_coef(1) = b;
      if (interp_order > 1) coef_coef(2) = c;
      if (interp_order > 2) coef_coef(3) = d;
      #undef coef_coef
      f2 = f1, f1 = f0;
    }
  return result;
}

typedef struct {    /* Data that are shared between channels and filters */
  sample_t   * poly_fir_coefs;
  dft_filter_t half_band[2];    /* [0]: halve; [1]: down/up: halve/double */
} rate_shared_t;

struct stage;
typedef void (* stage_fn_t)(struct stage * input, fifo_t * output);
typedef struct stage {
  rate_shared_t * shared;
  fifo_t     fifo;
  int        pre;              /* Number of past samples to store */
  int        pre_post;         /* pre + number of future samples to store */
  int        preload;          /* Number of zero samples to pre-load the fifo */
  int        which;            /* Which of the 2 half-band filters to use */
  stage_fn_t fn;
                               /* For poly_fir & spline: */
  union {                      /* 32bit.32bit fixed point arithmetic */
    #if defined(WORDS_BIGENDIAN)
    struct {int32_t integer; uint32_t fraction;} parts;
    #else
    struct {uint32_t fraction; int32_t integer;} parts;
    #endif
    int64_t all;
    #define MULT32 (65536. * 65536.)
  } at, step;
  int        divisor;          /* For step: > 1 for rational; 1 otherwise */
  double     out_in_ratio;
  fft_cache_t *cache;
} stage_t;

#define stage_occupancy(s) max(0, (int)fifo_occupancy(&(s)->fifo) - (s)->pre_post)
#define stage_read_p(s) ((sample_t *)fifo_read_ptr(&(s)->fifo) + (s)->pre)

static void cubic_spline(stage_t * p, fifo_t * output_fifo)
{
  int i, num_in = stage_occupancy(p), max_num_out = 1 + num_in*p->out_in_ratio;
  sample_t const * input = stage_read_p(p);
  sample_t * output = fifo_reserve(output_fifo, max_num_out);

  for (i = 0; p->at.parts.integer < num_in; ++i, p->at.all += p->step.all) {
    sample_t const * s = input + p->at.parts.integer;
    sample_t x = p->at.parts.fraction * (1 / MULT32);
    sample_t b = .5*(s[1]+s[-1])-*s, a = (1/6.)*(s[2]-s[1]+s[-1]-*s-4*b);
    sample_t c = s[1]-*s-a-b;
    output[i] = ((a*x + b)*x + c)*x + *s;
  }
  assert(max_num_out - i >= 0);
  fifo_trim_by(output_fifo, max_num_out - i);
  fifo_read(&p->fifo, p->at.parts.integer, NULL);
  p->at.parts.integer = 0;
}

static void half_sample(stage_t * p, fifo_t * output_fifo)
{
  sample_t * output;
  int i, j, num_in = max(0, fifo_occupancy(&p->fifo));
  rate_shared_t const * s = p->shared;
  dft_filter_t const * f = &s->half_band[p->which];
  int const overlap = f->num_taps - 1;

  while (num_in >= f->dft_length) {
    sample_t const * input = fifo_read_ptr(&p->fifo);
    fifo_read(&p->fifo, f->dft_length - overlap, NULL);
    num_in -= f->dft_length - overlap;

    output = fifo_reserve(output_fifo, f->dft_length);
    fifo_trim_by(output_fifo, (f->dft_length + overlap) >> 1);
    memcpy(output, input, f->dft_length * sizeof(*output));

    lsx_safe_rdft(f->dft_length, 1, output, p->cache);
    output[0] *= f->coefs[0];
    output[1] *= f->coefs[1];
    for (i = 2; i < f->dft_length; i += 2) {
      sample_t tmp = output[i];
      output[i  ] = f->coefs[i  ] * tmp - f->coefs[i+1] * output[i+1];
      output[i+1] = f->coefs[i+1] * tmp + f->coefs[i  ] * output[i+1];
    }
    lsx_safe_rdft(f->dft_length, -1, output, p->cache);

    for (j = 1, i = 2; i < f->dft_length - overlap; ++j, i += 2)
      output[j] = output[i];
  }
}

static void double_sample(stage_t * p, fifo_t * output_fifo)
{
  sample_t * output;
  int i, j, num_in = max(0, fifo_occupancy(&p->fifo));
  rate_shared_t const * s = p->shared;
  dft_filter_t const * f = &s->half_band[1];
  int const overlap = f->num_taps - 1;

  while (num_in > f->dft_length >> 1) {
    sample_t const * input = fifo_read_ptr(&p->fifo);
    fifo_read(&p->fifo, (f->dft_length - overlap) >> 1, NULL);
    num_in -= (f->dft_length - overlap) >> 1;

    output = fifo_reserve(output_fifo, f->dft_length);
    fifo_trim_by(output_fifo, overlap);
    for (j = i = 0; i < f->dft_length; ++j, i += 2)
      output[i] = input[j], output[i+1] = 0;

    lsx_safe_rdft(f->dft_length, 1, output, p->cache);
    output[0] *= f->coefs[0];
    output[1] *= f->coefs[1];
    for (i = 2; i < f->dft_length; i += 2) {
      sample_t tmp = output[i];
      output[i  ] = f->coefs[i  ] * tmp - f->coefs[i+1] * output[i+1];
      output[i+1] = f->coefs[i+1] * tmp + f->coefs[i  ] * output[i+1];
    }
    lsx_safe_rdft(f->dft_length, -1, output, p->cache);
  }
}

static void half_band_filter_init(rate_shared_t * p, unsigned which,
    int num_taps, sample_t const h[], double Fp, double att, int multiplier,
    double phase, sox_bool allow_aliasing, fft_cache_t *cache)
{
  dft_filter_t * f = &p->half_band[which];
  int dft_length, i;

  if (f->num_taps)
    return;
  if (h) {
    dft_length = lsx_set_dft_length(num_taps);
    f->coefs = lsx_calloc(dft_length, sizeof(*f->coefs));
    for (i = 0; i < num_taps; ++i)
      f->coefs[(i + dft_length - num_taps + 1) & (dft_length - 1)]
          = h[abs(num_taps / 2 - i)] / dft_length * 2 * multiplier;
    f->post_peak = num_taps / 2;
  }
  else {
    double * h2 = lsx_design_lpf(Fp, 1., 2., allow_aliasing, att, &num_taps, 0);

    if (phase != 50)
      lsx_fir_to_phase(&h2, &num_taps, &f->post_peak, phase, cache);
    else f->post_peak = num_taps / 2;

    dft_length = lsx_set_dft_length(num_taps);
    f->coefs = lsx_calloc(dft_length, sizeof(*f->coefs));
    for (i = 0; i < num_taps; ++i)
      f->coefs[(i + dft_length - num_taps + 1) & (dft_length - 1)]
          = h2[i] / dft_length * 2 * multiplier;
    free(h2);
  }
  assert(num_taps & 1);
  f->num_taps = num_taps;
  f->dft_length = dft_length;
  lsx_safe_rdft(dft_length, 1, f->coefs, cache);
}

#include "rate_filters.h"

typedef struct {
  double     factor;
  size_t     samples_in, samples_out;
  int        level, input_stage_num, output_stage_num;
  sox_bool   upsample;
  stage_t    * stages;
  fft_cache_t cache;
} rate_t;

#define pre_stage p->stages[-1]
#define last_stage p->stages[p->level]
#define post_stage p->stages[p->level + 1]

typedef enum {Default = -1, Quick, Low, Medium, High, Very} quality_t;

static void rate_init(rate_t * p, rate_shared_t * shared, double factor,
    quality_t quality, int interp_order, double phase, double bandwidth,
    sox_bool allow_aliasing)
{
  int i, mult, divisor = 1;

  assert(factor > 0);
  p->factor = factor;
  if (quality < Quick || quality > Very)
    quality = High;
  if (quality != Quick) {
    const int max_divisor = 2048;      /* Keep coef table size ~< 500kb */
    const double epsilon = 4 / MULT32; /* Scaled to half this at max_divisor */
    p->upsample = p->factor < 1;
    for (i = factor, p->level = 0; i >>= 1; ++p->level); /* log base 2 */
    factor /= 1 << (p->level + !p->upsample);
    for (i = 2; i <= max_divisor && divisor == 1; ++i) {
      double try_d = factor * i;
      int try = try_d + .5;
      if (fabs(try - try_d) < try * epsilon * (1 - (.5 / max_divisor) * i)) {
        if (try == i)  /* Rounded to 1:1? */
          factor = 1, divisor = 2, p->upsample = sox_false;
        else factor = try, divisor = i;
      }
    }
  }
  memset(&p->cache, 0, sizeof(p->cache));
  p->stages = (stage_t *)lsx_calloc((size_t)p->level + 4, sizeof(*p->stages)) + 1;
  for (i = -1; i <= p->level + 1; ++i) {
    p->stages[i].shared = shared;
    p->stages[i].cache = &p->cache;
  }
  last_stage.step.all = factor * MULT32 + .5;
  last_stage.out_in_ratio = MULT32 * divisor / last_stage.step.all;

  if (divisor != 1)
    assert(!last_stage.step.parts.fraction);
  else if (quality != Quick)
    assert(!last_stage.step.parts.integer);

  mult = 1 + p->upsample; /* Compensate for zero-stuffing in double_sample */
  p->input_stage_num = -p->upsample;
  p->output_stage_num = p->level;
  if (quality == Quick) {
    ++p->output_stage_num;
    last_stage.fn = cubic_spline;
    last_stage.pre_post = max(3, last_stage.step.parts.integer);
    last_stage.preload = last_stage.pre = 1;
  }
  else if (last_stage.out_in_ratio != 2 || (p->upsample && quality == Low)) {
    poly_fir_t const * f;
    poly_fir1_t const * f1;
    int n = 4 * p->upsample + range_limit(quality, Medium, Very) - Medium;
    if (interp_order < 0)
      interp_order = quality > High;
    interp_order = divisor == 1? 1 + interp_order : 0;
    last_stage.divisor = divisor;
    p->output_stage_num += 2;
    if (p->upsample && quality == Low)
      mult = 1, ++p->input_stage_num, --p->output_stage_num, --n;
    f = &poly_firs[n];
    f1 = &f->interp[interp_order];
    if (!last_stage.shared->poly_fir_coefs) {
      int num_taps = 0, phases = divisor == 1? (1 << f1->phase_bits) : divisor;
      raw_coef_t * coefs = lsx_design_lpf(
          f->pass, f->stop, 1., sox_false, f->att, &num_taps, phases);
      assert(num_taps == f->num_coefs * phases - 1);
      last_stage.shared->poly_fir_coefs =
          prepare_coefs(coefs, f->num_coefs, phases, interp_order, mult);
      free(coefs);
    }
    last_stage.fn = f1->fn;
    last_stage.pre_post = f->num_coefs - 1;
    last_stage.pre = 0;
    last_stage.preload = last_stage.pre_post >> 1;
    mult = 1;
  }
  if (quality > Low) {
    typedef struct {int len; sample_t const * h; double bw, a;} filter_t;
    static filter_t const filters[] = {
      {2 * array_length(half_fir_coefs_low) - 1, half_fir_coefs_low, 0,0},
      {0, NULL, .931, 110}, {0, NULL, .931, 125}, {0, NULL, .931, 170}};
    filter_t const * f = &filters[quality - Low];
    double att = allow_aliasing? (34./33)* f->a : f->a; /* negate att degrade */
    double bw = bandwidth? 1 - (1 - bandwidth / 100) / LSX_TO_3dB : f->bw;
    double min = 1 - (allow_aliasing? LSX_MAX_TBW0A : LSX_MAX_TBW0) / 100;
    assert((size_t)(quality - Low) < array_length(filters));
    half_band_filter_init(shared, p->upsample, f->len, f->h, bw, att, mult, phase, allow_aliasing, &p->cache);
    if (p->upsample) {
      pre_stage.fn = double_sample; /* Finish off setting up pre-stage */
      pre_stage.preload = shared->half_band[1].post_peak >> 1;
       /* Start setting up post-stage; TODO don't use dft for short filters */
      if ((1 - p->factor) / (1 - bw) > 2)
        half_band_filter_init(shared, 0, 0, NULL, max(p->factor, min), att, 1, phase, allow_aliasing, &p->cache);
      else shared->half_band[0] = shared->half_band[1];
    }
    else if (p->level > 0 && p->output_stage_num > p->level) {
      double pass = bw * divisor / factor / 2;
      if ((1 - pass) / (1 - bw) > 2)
        half_band_filter_init(shared, 1, 0, NULL, max(pass, min), att, 1, phase, allow_aliasing, &p->cache);
    }
    post_stage.fn = half_sample;
    post_stage.preload = shared->half_band[0].post_peak;
  }
  else if (quality == Low && !p->upsample) {    /* dft is slower here, so */
    post_stage.fn = half_sample_low;            /* use normal convolution */
    post_stage.pre_post = 2 * (array_length(half_fir_coefs_low) - 1);
    post_stage.preload = post_stage.pre = post_stage.pre_post >> 1;
  }
  if (p->level > 0) {
    stage_t * s = & p->stages[p->level - 1];
    if (shared->half_band[1].num_taps) {
      s->fn = half_sample;
      s->preload = shared->half_band[1].post_peak;
      s->which = 1;
    }
    else *s = post_stage;
  }
  for (i = p->input_stage_num; i <= p->output_stage_num; ++i) {
    stage_t * s = &p->stages[i];
    if (i >= 0 && i < p->level - 1) {
      s->fn = half_sample_25;
      s->pre_post = 2 * (array_length(half_fir_coefs_25) - 1);
      s->preload = s->pre = s->pre_post >> 1;
    }
    fifo_create(&s->fifo, (int)sizeof(sample_t));
    memset(fifo_reserve(&s->fifo, s->preload), 0, sizeof(sample_t)*s->preload);
  }
}

static void rate_process(rate_t * p)
{
  stage_t * stage = p->stages + p->input_stage_num;
  int i;

  for (i = p->input_stage_num; i < p->output_stage_num; ++i, ++stage)
    stage->fn(stage, &(stage+1)->fifo);
}

static sample_t * rate_input(rate_t * p, sample_t const * samples, size_t n)
{
  p->samples_in += n;
  return fifo_write(&p->stages[p->input_stage_num].fifo, (int)n, samples);
}

static sample_t const * rate_output(rate_t * p, sample_t * samples, size_t * n)
{
  fifo_t * fifo = &p->stages[p->output_stage_num].fifo;
  p->samples_out += *n = min(*n, (size_t)fifo_occupancy(fifo));
  return fifo_read(fifo, (int)*n, samples);
}

static void rate_flush(rate_t * p)
{
  fifo_t * fifo = &p->stages[p->output_stage_num].fifo;
  size_t samples_out = p->samples_in / p->factor + .5;
  size_t remaining = samples_out - p->samples_out;
  sample_t * buff = lsx_calloc(1024, sizeof(*buff));

  if ((int)remaining > 0) {
    while ((size_t)fifo_occupancy(fifo) < remaining) {
      rate_input(p, buff, (size_t) 1024);
      rate_process(p);
    }
    fifo_trim_to(fifo, (int)remaining);
    p->samples_in = 0;
  }
  free(buff);
}

static void rate_close(rate_t * p)
{
  rate_shared_t * shared = p->stages[0].shared;
  int i;

  for (i = p->input_stage_num; i <= p->output_stage_num; ++i)
    fifo_delete(&p->stages[i].fifo);
  free(shared->half_band[0].coefs);
  if (shared->half_band[1].coefs != shared->half_band[0].coefs)
    free(shared->half_band[1].coefs);
  free(shared->poly_fir_coefs);
  memset(shared, 0, sizeof(*shared));
  free(p->stages - 1);
  lsx_clear_fft_cache(&p->cache);
}

#include "module.h"
