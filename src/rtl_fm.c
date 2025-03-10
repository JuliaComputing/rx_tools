/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012 by Hoernchen <la@tfc-server.de>
 * Copyright (C) 2012 by Kyle Keen <keenerd@gmail.com>
 * Copyright (C) 2013 by Elias Oenal <EliasOenal@gmail.com>
 * Copyright (C) 2015 by Hayati Ayguen <h_ayguen@web.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


/*
 * written because people could not do real time
 * FM demod on Atom hardware with GNU radio
 * based on rtl_sdr.c and rtl_tcp.c
 *
 * lots of locks, but that is okay
 * (no many-to-many locks)
 *
 * todo:
 *	   sanity checks
 *	   scale squelch to other input parameters
 *	   test all the demodulations
 *	   pad output on hop
 *	   frequency ranges could be stored better
 *	   scaled AM demod amplification
 *	   auto-hop after time limit
 *	   peak detector to tune onto stronger signals
 *	   fifo for active hop frequency
 *	   clips
 *	   noise squelch
 *	   merge stereo patch
 *	   merge soft agc patch
 *	   merge udp patch
 *	   testmode to detect overruns
 *	   watchdog to reset bad dongle
 *	   fix oversampling
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include "getopt/getopt.h"
#define usleep(x) Sleep(x/1000)
#if defined(_MSC_VER) && (_MSC_VER < 1800)
#define round(x) (x > 0.0 ? floor(x + 0.5): ceil(x - 0.5))
#endif
#define _USE_MATH_DEFINES
#endif

#include <math.h>
#include <pthread.h>

#include "convenience.h"
#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>

#define DEFAULT_SAMPLE_RATE		24000
#define DEFAULT_BUF_LENGTH		(1 * 16384)
#define MAXIMUM_OVERSAMPLE		16
#define MAXIMUM_BUF_LENGTH		(MAXIMUM_OVERSAMPLE * DEFAULT_BUF_LENGTH)
#define BUFFER_DUMP				4096

#define FREQUENCIES_LIMIT		1000

static volatile int do_exit = 0;
static int lcm_post[17] = {1,1,1,3,1,5,3,7,1,9,5,11,3,13,7,15,1};
static int ACTUAL_BUF_LENGTH;

static int *atan_lut = NULL;
static int atan_lut_size = 131072; /* 512 KB */
static int atan_lut_coef = 8;

static int verbosity = 0;
static int printLevels = 0;
static int printLevelNo = 1;
static int levelMax = 0;
static int levelMaxMax = 0;
static double levelSum = 0.0;

static int tmp_stdout = -1;

struct dongle_state
{
	int	  exit_flag;
	pthread_t thread;
	SoapySDRDevice *dev;
	SoapySDRStream *stream;
	size_t channel;
	char	*dev_query;
	uint32_t freq;
	uint32_t rate;
	uint32_t bandwidth;
	char *gain_str;
	int16_t  buf16[MAXIMUM_BUF_LENGTH];
	int	  ppm_error;
	int	  offset_tuning;
	int	  direct_sampling;
	int	  mute;
	struct demod_state *demod_target;
};

struct demod_state
{
	int	  exit_flag;
	pthread_t thread;
	int16_t  lowpassed[MAXIMUM_BUF_LENGTH];
	int	  lp_len;
	int16_t  lp_i_hist[10][6];
	int16_t  lp_q_hist[10][6];
	int16_t  result[MAXIMUM_BUF_LENGTH];
	int16_t  droop_i_hist[9];
	int16_t  droop_q_hist[9];
	int	  result_len;
	int	  rate_in;
	int	  rate_out;
	int	  rate_out2;
	int	  now_r, now_j;
	int	  pre_r, pre_j;
	int	  prev_index;
	int	  downsample;	/* min 1, max 256 */
	int	  post_downsample;
	int	  output_scale;
	int	  squelch_level, conseq_squelch, squelch_hits, terminate_on_squelch, squelch_zero;
	int	  downsample_passes;
	int	  comp_fir_size;
	int	  custom_atan;
	int	  deemph, deemph_a;
	int	  now_lpr;
	int	  prev_lpr_index;
	int	  dc_block_audio, dc_avg, adc_block_const;
	int	  dc_block_raw, dc_avgI, dc_avgQ, rdc_block_const;
	void	 (*mode_demod)(struct demod_state*);
	pthread_rwlock_t rw;
	pthread_cond_t ready;
	pthread_mutex_t ready_m;
	struct output_state *output_target;
};

struct output_state
{
	int	  exit_flag;
	pthread_t thread;
	FILE	 *file;
	char	 *filename;
	int16_t  result[MAXIMUM_BUF_LENGTH];
	int	  result_len;
	int	  rate;
	int	  wav_format;
	pthread_rwlock_t rw;
	pthread_cond_t ready;
	pthread_mutex_t ready_m;
};

struct controller_state
{
	int	  exit_flag;
	pthread_t thread;
	uint32_t freqs[FREQUENCIES_LIMIT];
	int	  freq_len;
	int	  freq_now;
	int	  edge;
	int	  wb_mode;
	pthread_cond_t hop;
	pthread_mutex_t hop_m;
};

// multiple of these, eventually
struct dongle_state dongle;
struct demod_state demod;
struct output_state output;
struct controller_state controller;

void usage(void)
{
	fprintf(stderr,
		"rx_fm (based on rtl_fm), a simple narrow band FM demodulator for RTL2832 based DVB-T receivers\n\n"
		"Use:\trx_fm -f freq [-options] [filename]\n"
		"\t-f frequency_to_tune_to [Hz]\n"
		"\t	use multiple -f for scanning (requires squelch)\n"
		"\t	ranges supported, -f 118M:137M:25k\n"
		"\t[-v increase verbosity (default: 0)]\n"
		"\t[-M modulation (default: fm)]\n"
		"\t	fm or nbfm or nfm, wbfm or wfm, raw or iq, am, usb, lsb\n"
		"\t	wbfm == -M fm -s 170k -o 4 -A fast -r 32k -l 0 -E deemp\n"
		"\t	raw mode outputs 2x16 bit IQ pairs\n"
		"\t[-s sample_rate (default: 24k)]\n"
		"\t[-d device key/value query (ex: 0, 1, driver=rtlsdr, driver=hackrf)]\n"
		"\t[-g tuner gain(s) (ex: 20, 40, LNA=40,VGA=20,AMP=0)]\n"
		"\t[-w tuner_bandwidth (default: automatic. enables offset tuning)]\n"
		"\t[-C channel number (ex: 0)]\n"
		"\t[-a antenna (ex: 'Tuner 1 50 ohm')]\n"
		"\t[-l squelch_level (default: 0/off)]\n"
		"\t[-L N  prints levels every N calculations]\n"
		"\t	output are comma separated values (csv):\n"
		"\t	mean since last output, max since last output, overall max, squelch\n"
		"\t[-c de-emphasis_time_constant in us for wbfm. 'us' or 'eu' for 75/50 us (default: us)]\n"
		//"\t	for fm squelch is inverted\n"
		"\t[-o oversampling (default: 1, 4 recommended)]\n"
		"\t[-p ppm_error (default: 0)]\n"
		"\t[-E enable_option (default: none)]\n"
		"\t	use multiple -E to enable multiple options\n"
		"\t	edge:   enable lower edge tuning\n"
		"\t	rdc:    enable dc blocking filter on raw I/Q data at capture rate\n"
		"\t	adc:    enable dc blocking filter on demodulated audio\n"
		"\t	dc:     same as adc\n"
		"\t	rtlagc: enable rtl2832's digital agc (default: off)\n"
		"\t	agc:    same as rtlagc\n"
		"\t	deemp:  enable de-emphasis filter\n"
		"\t	direct: enable direct sampling (bypasses tuner, uses rtl2832 xtal)\n"
		"\t	no-mod: enable no-mod direct sampling\n"
		"\t	offset: enable offset tuning (only e4000 tuner)\n"
		"\t	zero:   emit zeros when squelch active\n"
		"\t	wav:    generate WAV header\n"
		"\t[-q dc_avg_factor for option rdc (default: 9)]\n"
		"\tfilename ('-' means stdout)\n"
		"\t	omitting the filename also uses stdout\n\n"
		"Experimental options:\n"
		"\t[-r resample_rate (default: none / same as -s)]\n"
		"\t[-t squelch_delay (default: 10)]\n"
		"\t	+values will mute/scan, -values will exit\n"
		"\t[-F fir_size (default: off)]\n"
		"\t	enables low-leakage downsample filter\n"
		"\t	size can be 0 or 9.  0 has bad roll off\n"
		"\t[-A std/fast/lut/ale choose atan math (default: std)]\n"
		//"\t[-C clip_path (default: off)\n"
		//"\t (create time stamped raw clips, requires squelch)\n"
		//"\t (path must have '\%s' and will expand to date_time_freq)\n"
		//"\t[-H hop_fifo (default: off)\n"
		//"\t (fifo will contain the active frequency)\n"
		"\n"
		"Produces signed 16 bit ints, use Sox or aplay to hear them.\n"
		"\trx_fm ... | play -t raw -r 24k -es -b 16 -c 1 -V1 -\n"
		"\t		   | aplay -r 24k -f S16_LE -t raw -c 1\n"
		"\t  -M wbfm  | play -r 32k ... \n"
		"\t  -E wav   | play -t wav - \n"
		"\t  -s 22050 | multimon -t raw /dev/stdin\n\n");
	exit(1);
}

#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		do_exit = 1;
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum)
{
	fprintf(stderr, "Signal caught, exiting!\n");
	do_exit = 1;
}
#endif

/* more cond dumbness */
#define safe_cond_signal(n, m) pthread_mutex_lock(m); pthread_cond_signal(n); pthread_mutex_unlock(m)
#define safe_cond_wait(n, m) pthread_mutex_lock(m); pthread_cond_wait(n, m); pthread_mutex_unlock(m)

/* {length, coef, coef, coef}  and scaled by 2^15
   for now, only length 9, optimal way to get +85% bandwidth */
#define CIC_TABLE_MAX 10
int cic_9_tables[][10] = {
	{0,},
	{9, -156,  -97, 2798, -15489, 61019, -15489, 2798,  -97, -156},
	{9, -128, -568, 5593, -24125, 74126, -24125, 5593, -568, -128},
	{9, -129, -639, 6187, -26281, 77511, -26281, 6187, -639, -129},
	{9, -122, -612, 6082, -26353, 77818, -26353, 6082, -612, -122},
	{9, -120, -602, 6015, -26269, 77757, -26269, 6015, -602, -120},
	{9, -120, -582, 5951, -26128, 77542, -26128, 5951, -582, -120},
	{9, -119, -580, 5931, -26094, 77505, -26094, 5931, -580, -119},
	{9, -119, -578, 5921, -26077, 77484, -26077, 5921, -578, -119},
	{9, -119, -577, 5917, -26067, 77473, -26067, 5917, -577, -119},
	{9, -199, -362, 5303, -25505, 77489, -25505, 5303, -362, -199},
};

#if defined(_MSC_VER) && (_MSC_VER < 1800)
double log2(double n)
{
	return log(n) / log(2.0);
}
#endif

void rotate16_90(int16_t *buf, uint32_t len)
/* 90 rotation is 1+0j, 0+1j, -1+0j, 0-1j
   or [0, 1, -3, 2, -4, -5, 7, -6] */
{
	uint32_t i;
	int16_t tmp;
	for (i=0; i<len; i+=8) {
		tmp = - buf[i+3];
		buf[i+3] = buf[i+2];
		buf[i+2] = tmp;

		buf[i+4] = - buf[i+4];
		buf[i+5] = - buf[i+5];

		tmp = - buf[i+6];
		buf[i+6] = buf[i+7];
		buf[i+7] = tmp;
	}
}


void rotate_90(unsigned char *buf, uint32_t len)
/* 90 rotation is 1+0j, 0+1j, -1+0j, 0-1j
   or [0, 1, -3, 2, -4, -5, 7, -6] */
{
	uint32_t i;
	unsigned char tmp;
	for (i=0; i<len; i+=8) {
		/* uint8_t negation = 255 - x */
		tmp = 255 - buf[i+3];
		buf[i+3] = buf[i+2];
		buf[i+2] = tmp;

		buf[i+4] = 255 - buf[i+4];
		buf[i+5] = 255 - buf[i+5];

		tmp = 255 - buf[i+6];
		buf[i+6] = buf[i+7];
		buf[i+7] = tmp;
	}
}

void low_pass(struct demod_state *d)
/* simple square window FIR */
{
	int i=0, i2=0;
	while (i < d->lp_len) {
		d->now_r += d->lowpassed[i];
		d->now_j += d->lowpassed[i+1];
		i += 2;
		d->prev_index++;
		if (d->prev_index < d->downsample) {
			continue;
		}
		d->lowpassed[i2]   = d->now_r; // * d->output_scale;
		d->lowpassed[i2+1] = d->now_j; // * d->output_scale;
		d->prev_index = 0;
		d->now_r = 0;
		d->now_j = 0;
		i2 += 2;
	}
	d->lp_len = i2;
}

int low_pass_simple(int16_t *signal2, int len, int step)
// no wrap around, length must be multiple of step
{
	int i, i2, sum;
	for(i=0; i < len; i+=step) {
		sum = 0;
		for(i2=0; i2<step; i2++) {
			sum += (int)signal2[i + i2];
		}
		//signal2[i/step] = (int16_t)(sum / step);
		signal2[i/step] = (int16_t)(sum);
	}
	signal2[i/step + 1] = signal2[i/step];
	return len / step;
}

void low_pass_real(struct demod_state *s)
/* simple square window FIR */
// add support for upsampling?
{
	int i=0, i2=0;
	int fast = (int)s->rate_out;
	int slow = s->rate_out2;
	while (i < s->result_len) {
		s->now_lpr += s->result[i];
		i++;
		s->prev_lpr_index += slow;
		if (s->prev_lpr_index < fast) {
			continue;
		}
		s->result[i2] = (int16_t)(s->now_lpr / (fast/slow));
		s->prev_lpr_index -= fast;
		s->now_lpr = 0;
		i2 += 1;
	}
	s->result_len = i2;
}

void fifth_order(int16_t *data, int length, int16_t *hist)
/* for half of interleaved data */
{
	int i;
	int16_t a, b, c, d, e, f;
	a = hist[1];
	b = hist[2];
	c = hist[3];
	d = hist[4];
	e = hist[5];
	f = data[0];
	/* a downsample should improve resolution, so don't fully shift */
	data[0] = (a + (b+e)*5 + (c+d)*10 + f) >> 4;
	for (i=4; i<length; i+=4) {
		a = c;
		b = d;
		c = e;
		d = f;
		e = data[i-2];
		f = data[i];
		data[i/2] = (a + (b+e)*5 + (c+d)*10 + f) >> 4;
	}
	/* archive */
	hist[0] = a;
	hist[1] = b;
	hist[2] = c;
	hist[3] = d;
	hist[4] = e;
	hist[5] = f;
}

void generic_fir(int16_t *data, int length, int *fir, int16_t *hist)
/* Okay, not at all generic.  Assumes length 9, fix that eventually. */
{
	int d, temp, sum;
	for (d=0; d<length; d+=2) {
		temp = data[d];
		sum = 0;
		sum += (hist[0] + hist[8]) * fir[1];
		sum += (hist[1] + hist[7]) * fir[2];
		sum += (hist[2] + hist[6]) * fir[3];
		sum += (hist[3] + hist[5]) * fir[4];
		sum +=			hist[4]  * fir[5];
		data[d] = sum >> 15 ;
		hist[0] = hist[1];
		hist[1] = hist[2];
		hist[2] = hist[3];
		hist[3] = hist[4];
		hist[4] = hist[5];
		hist[5] = hist[6];
		hist[6] = hist[7];
		hist[7] = hist[8];
		hist[8] = temp;
	}
}

/* define our own complex math ops
   because ARMv5 has no hardware float */

void multiply(int ar, int aj, int br, int bj, int *cr, int *cj)
{
	*cr = ar*br - aj*bj;
	*cj = aj*br + ar*bj;
}

int polar_discriminant(int ar, int aj, int br, int bj)
{
	int cr, cj;
	double angle;
	multiply(ar, aj, br, -bj, &cr, &cj);
	angle = atan2((double)cj, (double)cr);
	return (int)(angle / 3.14159 * (1<<14));
}

int fast_atan2(int y, int x)
/* pre scaled for int16 */
{
	int yabs, angle;
	int pi4=(1<<12), pi34=3*(1<<12);  // note pi = 1<<14
	if (x==0 && y==0) {
		return 0;
	}
	yabs = y;
	if (yabs < 0) {
		yabs = -yabs;
	}
	if (x >= 0) {
		angle = pi4  - pi4 * (x-yabs) / (x+yabs);
	} else {
		angle = pi34 - pi4 * (x+yabs) / (yabs-x);
	}
	if (y < 0) {
		return -angle;
	}
	return angle;
}

int polar_disc_fast(int ar, int aj, int br, int bj)
{
	int cr, cj;
	multiply(ar, aj, br, -bj, &cr, &cj);
	return fast_atan2(cj, cr);
}

int atan_lut_init(void)
{
	int i = 0;

	atan_lut = malloc(atan_lut_size * sizeof(int));

	for (i = 0; i < atan_lut_size; i++) {
		atan_lut[i] = (int) (atan((double) i / (1<<atan_lut_coef)) / 3.14159 * (1<<14));
	}

	return 0;
}

int polar_disc_lut(int ar, int aj, int br, int bj)
{
	int cr, cj, x, x_abs;

	multiply(ar, aj, br, -bj, &cr, &cj);

	/* special cases */
	if (cr == 0 || cj == 0) {
		if (cr == 0 && cj == 0)
			{return 0;}
		if (cr == 0 && cj > 0)
			{return 1 << 13;}
		if (cr == 0 && cj < 0)
			{return -(1 << 13);}
		if (cj == 0 && cr > 0)
			{return 0;}
		if (cj == 0 && cr < 0)
			{return 1 << 14;}
	}

	/* real range -32768 - 32768 use 64x range -> absolute maximum: 2097152 */
	x = (cj << atan_lut_coef) / cr;
	x_abs = abs(x);

	if (x_abs >= atan_lut_size) {
		/* we can use linear range, but it is not necessary */
		return (cj > 0) ? 1<<13 : -(1<<13);
	}

	if (x > 0) {
		return (cj > 0) ? atan_lut[x] : atan_lut[x] - (1<<14);
	} else {
		return (cj > 0) ? (1<<14) - atan_lut[-x] : -atan_lut[-x];
	}

	return 0;
}

int esbensen(int ar, int aj, int br, int bj)
/*
  input signal: s(t) = a*exp(-i*w*t+p)
  a = amplitude, w = angular freq, p = phase difference
  solve w
  s' = -i(w)*a*exp(-i*w*t+p)
  s'*conj(s) = -i*w*a*a
  s'*conj(s) / |s|^2 = -i*w
*/
{
	int cj, dr, dj;
	int scaled_pi = 2608; /* 1<<14 / (2*pi) */
	dr = (br - ar) * 2;
	dj = (bj - aj) * 2;
	cj = bj*dr - br*dj; /* imag(ds*conj(s)) */
	return (scaled_pi * cj / (ar*ar+aj*aj+1));
}

void fm_demod(struct demod_state *fm)
{
	int i, pcm;
	int16_t *lp = fm->lowpassed;
	pcm = polar_discriminant(lp[0], lp[1],
		fm->pre_r, fm->pre_j);
	fm->result[0] = (int16_t)pcm;
	for (i = 2; i < (fm->lp_len-1); i += 2) {
		switch (fm->custom_atan) {
		case 0:
			pcm = polar_discriminant(lp[i], lp[i+1],
				lp[i-2], lp[i-1]);
			break;
		case 1:
			pcm = polar_disc_fast(lp[i], lp[i+1],
				lp[i-2], lp[i-1]);
			break;
		case 2:
			pcm = polar_disc_lut(lp[i], lp[i+1],
				lp[i-2], lp[i-1]);
			break;
		case 3:
			pcm = esbensen(lp[i], lp[i+1],
				lp[i-2], lp[i-1]);
			break;
		}
		fm->result[i/2] = (int16_t)pcm;
	}
	fm->pre_r = lp[fm->lp_len - 2];
	fm->pre_j = lp[fm->lp_len - 1];
	fm->result_len = fm->lp_len/2;
}

void am_demod(struct demod_state *fm)
// todo, fix this extreme laziness
{
	int i, pcm;
	int16_t *lp = fm->lowpassed;
	int16_t *r  = fm->result;
	for (i = 0; i < fm->lp_len; i += 2) {
		// hypot uses floats but won't overflow
		//r[i/2] = (int16_t)hypot(lp[i], lp[i+1]);
		pcm = lp[i] * lp[i];
		pcm += lp[i+1] * lp[i+1];
		r[i/2] = (int16_t)sqrt(pcm) * fm->output_scale;
	}
	fm->result_len = fm->lp_len/2;
	// lowpass? (3khz)  highpass?  (dc)
}

void usb_demod(struct demod_state *fm)
{
	int i, pcm;
	int16_t *lp = fm->lowpassed;
	int16_t *r  = fm->result;
	for (i = 0; i < fm->lp_len; i += 2) {
		pcm = lp[i] + lp[i+1];
		r[i/2] = (int16_t)pcm * fm->output_scale;
	}
	fm->result_len = fm->lp_len/2;
}

void lsb_demod(struct demod_state *fm)
{
	int i, pcm;
	int16_t *lp = fm->lowpassed;
	int16_t *r  = fm->result;
	for (i = 0; i < fm->lp_len; i += 2) {
		pcm = lp[i] - lp[i+1];
		r[i/2] = (int16_t)pcm * fm->output_scale;
	}
	fm->result_len = fm->lp_len/2;
}

void raw_demod(struct demod_state *fm)
{
	int i;
	for (i = 0; i < fm->lp_len; i++) {
		fm->result[i] = (int16_t)fm->lowpassed[i];
	}
	fm->result_len = fm->lp_len;
}

void deemph_filter(struct demod_state *fm)
{
	static int avg;  // cheating...
	int i, d;
	// de-emph IIR
	// avg = avg * (1 - alpha) + sample * alpha;
	for (i = 0; i < fm->result_len; i++) {
		d = fm->result[i] - avg;
		if (d > 0) {
			avg += (d + fm->deemph_a/2) / fm->deemph_a;
		} else {
			avg += (d - fm->deemph_a/2) / fm->deemph_a;
		}
		fm->result[i] = (int16_t)avg;
	}
}

void dc_block_audio_filter(struct demod_state *fm)
{
	int i, avg;
	int64_t sum = 0;
	for (i=0; i < fm->result_len; i++) {
		sum += fm->result[i];
	}
	avg = sum / fm->result_len;
	avg = (avg + fm->dc_avg * fm->adc_block_const) / ( fm->adc_block_const + 1 );
	for (i=0; i < fm->result_len; i++) {
		fm->result[i] -= avg;
	}
	fm->dc_avg = avg;
}

void dc_block_raw_filter(struct demod_state *fm, int16_t *buf, int len)
{
	/* derived from dc_block_audio_filter,
		running over the raw I/Q components
	*/
	int i, avgI, avgQ;
	int64_t sumI = 0;
	int64_t sumQ = 0;
	for (i = 0; i < len; i += 2) {
		sumI += buf[i];
		sumQ += buf[i+1];
	}
	avgI = sumI / ( len / 2 );
	avgQ = sumQ / ( len / 2 );
	avgI = (avgI + fm->dc_avgI * fm->rdc_block_const) / ( fm->rdc_block_const + 1 );
	avgQ = (avgQ + fm->dc_avgQ * fm->rdc_block_const) / ( fm->rdc_block_const + 1 );
	for (i = 0; i < len; i += 2) {
		buf[i] -= avgI;
		buf[i+1] -= avgQ;
	}
	fm->dc_avgI = avgI;
	fm->dc_avgQ = avgQ;
}
int mad(int16_t *samples, int len, int step)
/* mean average deviation */
{
	int i=0, sum=0, ave=0;
	if (len == 0)
		{return 0;}
	for (i=0; i<len; i+=step) {
		sum += samples[i];
	}
	ave = sum / (len * step);
	sum = 0;
	for (i=0; i<len; i+=step) {
		sum += abs(samples[i] - ave);
	}
	return sum / (len / step);
}

int rms(int16_t *samples, int len, int step)
/* largely lifted from rtl_power */
{
	int i;
	long p, t, s;
	double dc, err;

	p = t = 0L;
	for (i=0; i<len; i+=step) {
		s = (long)samples[i];
		t += s;
		p += s * s;
	}
	/* correct for dc offset in squares */
	dc = (double)(t*step) / (double)len;
	err = t * 2 * dc - dc * dc * len;

	return (int)sqrt((p-err) / len);
}

void full_demod(struct demod_state *d)
{
	int i, ds_p;
	int sr = 0;
	ds_p = d->downsample_passes;
	if (ds_p) {
		for (i=0; i < ds_p; i++) {
			fifth_order(d->lowpassed,   (d->lp_len >> i), d->lp_i_hist[i]);
			fifth_order(d->lowpassed+1, (d->lp_len >> i) - 1, d->lp_q_hist[i]);
		}
		d->lp_len = d->lp_len >> ds_p;
		/* droop compensation */
		if (d->comp_fir_size == 9 && ds_p <= CIC_TABLE_MAX) {
			generic_fir(d->lowpassed, d->lp_len,
				cic_9_tables[ds_p], d->droop_i_hist);
			generic_fir(d->lowpassed+1, d->lp_len-1,
				cic_9_tables[ds_p], d->droop_q_hist);
		}
	} else {
		low_pass(d);
	}
	/* power squelch */
	if (d->squelch_level) {
		sr = rms(d->lowpassed, d->lp_len, 1);
		if (sr < d->squelch_level) {
			d->squelch_hits++;
			for (i=0; i<d->lp_len; i++) {
				d->lowpassed[i] = 0;
			}
		} else {
			d->squelch_hits = 0;}
	}

	if (printLevels) {
		if (!sr)
			sr = rms(d->lowpassed, d->lp_len, 1);
		--printLevelNo;
		if (printLevels) {
			levelSum += sr;
			if (levelMax < sr)		levelMax = sr;
			if (levelMaxMax < sr)	levelMaxMax = sr;
			if  (!printLevelNo) {
				printLevelNo = printLevels;
				fprintf(stderr, "%f, %d, %d, %d\n", (levelSum / printLevels), levelMax, levelMaxMax, d->squelch_level );
				levelMax = 0;
				levelSum = 0;
			}
		}
	}
	d->mode_demod(d);  /* lowpassed -> result */
	if (d->mode_demod == &raw_demod) {
		return;
	}
	/* todo, fm noise squelch */
	// use nicer filter here too?
	if (d->post_downsample > 1) {
		d->result_len = low_pass_simple(d->result, d->result_len, d->post_downsample);}
	if (d->deemph) {
		deemph_filter(d);}
	if (d->dc_block_audio) {
		dc_block_audio_filter(d);}
	if (d->rate_out2 > 0) {
		low_pass_real(d);
		//arbitrary_resample(d->result, d->result, d->result_len, d->result_len * d->rate_out2 / d->rate_out);
	}
}

// buf: buffer
// len: number of elements in buf
static void rtlsdr_callback(int16_t *buf, uint32_t len, void *ctx)
{
	int i;
	struct dongle_state *s = ctx;
	struct demod_state *d;

	if (do_exit) {
		return;}
	if (!s) {
		return;}
	d  = s->demod_target;
	if (s->mute) {
		for (i=0; i<s->mute; i++) {
			buf[i] = 0;}
		s->mute = 0;
	}
	/* 1st: convert to 16 bit - to allow easier calculation of DC */
	for (i=0; i<(int)len; i++) {
		s->buf16[i] = ( (int16_t)buf[i] / 32767.0 * 128.0 + 0.4);
		// TODO: remove downconversion from 16-bit to 8-bit
	}
	/* 2nd: do DC filtering BEFORE up-mixing */
	if (d->dc_block_raw) {
		dc_block_raw_filter(d, s->buf16, (int)len);
	}
	/* 3rd: up-mixing */
	if (!s->offset_tuning) {
		rotate16_90(s->buf16, (int)len);
		/* rotate_90(buf, len); */
	}
	pthread_rwlock_wrlock(&d->rw);
	memcpy(d->lowpassed, s->buf16, 2*len);
	d->lp_len = len;
	pthread_rwlock_unlock(&d->rw);
	safe_cond_signal(&d->ready, &d->ready_m);
}

int generate_header(struct demod_state *d, struct output_state *o);
static void *dongle_thread_fn(void *arg)
{
	struct dongle_state *s = arg;

	SoapySDRDevice_activateStream(s->dev, s->stream, 0, 0, 0);
	size_t samples_per_buffer = MAXIMUM_BUF_LENGTH/2; //fix for int16 storage
	size_t elemsize = SoapySDR_formatToSize(SOAPY_SDR_CS16);
	int16_t *buf = malloc(samples_per_buffer * elemsize);
	memset(buf, 0, samples_per_buffer * elemsize);
	if (!buf) {
		perror("malloc");
		exit(1);
	}

	suppress_stdout_stop(tmp_stdout);

	if (output.wav_format) {
		generate_header(&demod, &output);
	}

	int r = 0;
	do
	{
		void *buffs[] = {buf};
		int flags = 0;
		long long timeNs = 0;
		long timeoutNs = 1000000;

		r = SoapySDRDevice_readStream(s->dev, s->stream, buffs, samples_per_buffer, &flags, &timeNs, timeoutNs);
		//fprintf(stderr, "ret=%d\n", r);

		if (r >= 0) {
			// r is number of elements read, elements=complex pairs, so buffer length in bytes is twice
			rtlsdr_callback(buf, r * 2, s);
		} else {
			if (r == SOAPY_SDR_OVERFLOW) {
				fprintf(stderr, "O");
				fflush(stderr);
				continue;
			}
			fprintf(stderr, "readStream read failed: %d\n", r);
			break;
		}
	} while(1);
	fprintf(stderr, "dongle_thread_fn terminated\n");

	//rtlsdr_read_async(s->dev, rtlsdr_callback, s, 0, s->buf_len);
	return 0;
}

static void *demod_thread_fn(void *arg)
{
	struct demod_state *d = arg;
	struct output_state *o = d->output_target;
	while (!do_exit) {
		safe_cond_wait(&d->ready, &d->ready_m);
		pthread_rwlock_wrlock(&d->rw);
		full_demod(d);
		pthread_rwlock_unlock(&d->rw);
		if (d->exit_flag) {
			do_exit = 1;
		}
		bool squelch_active = (d->squelch_level && d->squelch_hits > d->conseq_squelch);
		if (squelch_active && !d->squelch_zero) {
			d->squelch_hits = d->conseq_squelch + 1;  /* hair trigger */
			safe_cond_signal(&controller.hop, &controller.hop_m);
			continue;
		}
		pthread_rwlock_wrlock(&o->rw);
		if (squelch_active && d->squelch_zero) {
			memset(o->result, 0, 2*d->result_len);
		} else {
			memcpy(o->result, d->result, 2*d->result_len);
		}
		o->result_len = d->result_len;
		pthread_rwlock_unlock(&o->rw);
		safe_cond_signal(&o->ready, &o->ready_m);
	}
	return 0;
}

static void *output_thread_fn(void *arg)
{
	struct output_state *s = arg;
	while (!do_exit) {
		// use timedwait and pad out under runs
		safe_cond_wait(&s->ready, &s->ready_m);
		pthread_rwlock_rdlock(&s->rw);
		fwrite(s->result, 2, s->result_len, s->file);
		pthread_rwlock_unlock(&s->rw);
	}
	return 0;
}

static void optimal_settings(int freq, int rate)
{
	// giant ball of hacks
	// seems unable to do a single pass, 2:1
	int capture_freq, capture_rate;
	struct dongle_state *d = &dongle;
	struct demod_state *dm = &demod;
	struct controller_state *cs = &controller;
	dm->downsample = (1000000 / dm->rate_in) + 1;
	if (dm->downsample_passes) {
		dm->downsample_passes = (int)log2(dm->downsample) + 1;
		dm->downsample = 1 << dm->downsample_passes;
	}
	if (verbosity) {
		fprintf(stderr, "downsample_passes = %d (= # of fifth_order() iterations), downsample = %d\n", dm->downsample_passes, dm->downsample );
	}
	capture_freq = freq;
	capture_rate = dm->downsample * dm->rate_in;
	if (verbosity)
		fprintf(stderr, "capture_rate = dm->downsample * dm->rate_in = %d * %d = %d\n", dm->downsample, dm->rate_in, capture_rate );
	if (!d->offset_tuning) {
		capture_freq = freq + capture_rate/4;
		if (verbosity)
			fprintf(stderr, "optimal_settings(freq = %d): capture_freq = freq + capture_rate/4 = %d\n", freq, capture_freq );
	}
	capture_freq += cs->edge * dm->rate_in / 2;
	if (verbosity)
		fprintf(stderr, "optimal_settings(freq = %d): capture_freq +=  cs->edge * dm->rate_in / 2 = %d * %d / 2 = %d\n", freq, cs->edge, dm->rate_in, capture_freq );
	dm->output_scale = (1<<15) / (128 * dm->downsample);
	if (dm->output_scale < 1) {
		dm->output_scale = 1;}
	if (dm->mode_demod == &fm_demod) {
		dm->output_scale = 1;}
	d->freq = (uint32_t)capture_freq;
	d->rate = (uint32_t)capture_rate;
	if (verbosity)
		fprintf(stderr, "optimal_settings(freq = %d) delivers freq %.0f, rate %.0f\n", freq, (double)d->freq, (double)d->rate );
}

static void *controller_thread_fn(void *arg)
{
	// thoughts for multiple dongles
	// might be no good using a controller thread if retune/rate blocks
	int i;
	struct controller_state *s = arg;

	if (s->wb_mode) {
		if (verbosity)
			fprintf(stderr, "wbfm: adding 16000 Hz to every input frequency\n");
		for (i=0; i < s->freq_len; i++) {
			s->freqs[i] += 16000;}
	}

	/* set up primary channel */
	optimal_settings(s->freqs[0], demod.rate_in);
	if (dongle.direct_sampling) {
		verbose_direct_sampling(dongle.dev, dongle.direct_sampling);}
	if (dongle.offset_tuning) {
		verbose_offset_tuning(dongle.dev);}

	/* Set the frequency */
	if (verbosity) {
		fprintf(stderr, "verbose_set_frequency(%.0f Hz)\n", (double)dongle.freq);
		if (!dongle.offset_tuning)
			fprintf(stderr, "  frequency is away from parametrized one, to avoid negative impact from dc\n");
	}
	verbose_set_frequency(dongle.dev, dongle.freq, dongle.channel);
	fprintf(stderr, "Oversampling input by: %ix.\n", demod.downsample);
	fprintf(stderr, "Oversampling output by: %ix.\n", demod.post_downsample);
	fprintf(stderr, "Buffer size: %0.2fms\n",
		1000 * 0.5 * (float)ACTUAL_BUF_LENGTH / (float)dongle.rate);

	/* Set the sample rate */
	if (verbosity)
		fprintf(stderr, "verbose_set_sample_rate(%.0f Hz)\n", (double)dongle.rate);
	verbose_set_sample_rate(dongle.dev, dongle.rate, dongle.channel);
	fprintf(stderr, "Output at %u Hz.\n", demod.rate_in/demod.post_downsample);

	SoapySDRKwargs args = {0};
	while (!do_exit) {
		safe_cond_wait(&s->hop, &s->hop_m);
		if (s->freq_len <= 1) {
			continue;}
		/* hacky hopping */
		s->freq_now = (s->freq_now + 1) % s->freq_len;
		optimal_settings(s->freqs[s->freq_now], demod.rate_in);
		SoapySDRDevice_setFrequency(dongle.dev, SOAPY_SDR_RX, 0, (double)dongle.freq, &args);
		dongle.mute = BUFFER_DUMP;
	}
	return 0;
}

void frequency_range(struct controller_state *s, char *arg)
{
	char *start, *stop, *step;
	int i;
	start = arg;
	stop = strchr(start, ':') + 1;
	stop[-1] = '\0';
	step = strchr(stop, ':') + 1;
	step[-1] = '\0';
	for(i=(int)atofs(start); i<=(int)atofs(stop); i+=(int)atofs(step))
	{
		s->freqs[s->freq_len] = (uint32_t)i;
		s->freq_len++;
		if (s->freq_len >= FREQUENCIES_LIMIT) {
			break;}
	}
	stop[-1] = ':';
	step[-1] = ':';
}

void dongle_init(struct dongle_state *s)
{
	s->rate = DEFAULT_SAMPLE_RATE;
	s->gain_str = NULL;
	s->mute = 0;
	s->direct_sampling = 0;
	s->offset_tuning = 0;
	s->demod_target = &demod;
	s->bandwidth = 0;
	s->channel = 0;
}

void demod_init(struct demod_state *s)
{
	s->rate_in = DEFAULT_SAMPLE_RATE;
	s->rate_out = DEFAULT_SAMPLE_RATE;
	s->squelch_level = 0;
	s->conseq_squelch = 10;
	s->terminate_on_squelch = 0;
	s->squelch_hits = 11;
	s->downsample_passes = 0;
	s->comp_fir_size = 0;
	s->prev_index = 0;
	s->post_downsample = 1;	// once this works, default = 4
	s->custom_atan = 0;
	s->deemph = 0;
	s->rate_out2 = -1;	// flag for disabled
	s->mode_demod = &fm_demod;
	s->pre_j = s->pre_r = s->now_r = s->now_j = 0;
	s->prev_lpr_index = 0;
	s->deemph_a = 0;
	s->now_lpr = 0;
	s->dc_block_audio = 0;
	s->dc_avg = 0;
	s->adc_block_const = 9;
	s->dc_block_raw = 0;
	s->dc_avgI = 0;
	s->dc_avgQ = 0;
	s->rdc_block_const = 9;
	pthread_rwlock_init(&s->rw, NULL);
	pthread_cond_init(&s->ready, NULL);
	pthread_mutex_init(&s->ready_m, NULL);
	s->output_target = &output;
}

void demod_cleanup(struct demod_state *s)
{
	pthread_rwlock_destroy(&s->rw);
	pthread_cond_destroy(&s->ready);
	pthread_mutex_destroy(&s->ready_m);
}

void output_init(struct output_state *s)
{
	//s->rate = DEFAULT_SAMPLE_RATE;
	pthread_rwlock_init(&s->rw, NULL);
	pthread_cond_init(&s->ready, NULL);
	pthread_mutex_init(&s->ready_m, NULL);
}

void output_cleanup(struct output_state *s)
{
	pthread_rwlock_destroy(&s->rw);
	pthread_cond_destroy(&s->ready);
	pthread_mutex_destroy(&s->ready_m);
}

void controller_init(struct controller_state *s)
{
	s->freqs[0] = 100000000;
	s->freq_len = 0;
	s->edge = 0;
	s->wb_mode = 0;
	pthread_cond_init(&s->hop, NULL);
	pthread_mutex_init(&s->hop_m, NULL);
}

void controller_cleanup(struct controller_state *s)
{
	pthread_cond_destroy(&s->hop);
	pthread_mutex_destroy(&s->hop_m);
}

void sanity_checks(void)
{
	if (controller.freq_len == 0) {
		fprintf(stderr, "Please specify a frequency.\n");
		usage();
	}

	if (controller.freq_len >= FREQUENCIES_LIMIT) {
		fprintf(stderr, "Too many channels, maximum %i.\n", FREQUENCIES_LIMIT);
		exit(1);
	}

	if (controller.freq_len > 1 && demod.squelch_level == 0) {
		fprintf(stderr, "Please specify a squelch level.  Required for scanning multiple frequencies.\n");
		exit(1);
	}

}

int generate_header(struct demod_state *d, struct output_state *o)
{
	int i, s_rate, b_rate;
	char *channels = "\1\0";
	char *align = "\2\0";
	uint8_t samp_rate[4] = {0, 0, 0, 0};
	uint8_t byte_rate[4] = {0, 0, 0, 0};
	s_rate = o->rate;
	b_rate = o->rate * 2;
	if (d->mode_demod == &raw_demod) {
		channels = "\2\0";
		align = "\4\0";
		b_rate *= 2;
	}
	for (i=0; i<4; i++) {
		samp_rate[i] = (uint8_t)((s_rate >> (8*i)) & 0xFF);
		byte_rate[i] = (uint8_t)((b_rate >> (8*i)) & 0xFF);
	}
	fwrite("RIFF",     1, 4, o->file);
	fwrite("\xFF\xFF\xFF\xFF", 1, 4, o->file);  /* size */
	fwrite("WAVE",     1, 4, o->file);
	fwrite("fmt ",     1, 4, o->file);
	fwrite("\x10\0\0\0", 1, 4, o->file);  /* size */
	fwrite("\1\0",     1, 2, o->file);  /* pcm */
	fwrite(channels,   1, 2, o->file);
	fwrite(samp_rate,  1, 4, o->file);
	fwrite(byte_rate,  1, 4, o->file);
	fwrite(align, 1, 2, o->file);
	fwrite("\x10\0",     1, 2, o->file);  /* bits per channel */
	fwrite("data",     1, 4, o->file);
	fwrite("\xFF\xFF\xFF\xFF", 1, 4, o->file);  /* size */
	return 0;
}

int main(int argc, char **argv)
{
#ifndef _WIN32
	struct sigaction sigact;
#endif
	int custom_ppm = 0;
	int r, opt;
	int timeConstant = 75; /* default: U.S. 75 uS */
	int rtlagc = 0;
	char *antenna_str = NULL;
	dongle_init(&dongle);
	demod_init(&demod);
	output_init(&output);
	controller_init(&controller);
	dongle.dev_query = "";

	while ((opt = getopt(argc, argv, "a:C:d:f:g:s:b:l:L:o:t:r:p:E:q:F:A:M:c:h:w:v")) != -1) {
		switch (opt) {
		case 'a':
			antenna_str = optarg;
			break;
		case 'C':
			dongle.channel = (int)atoi(optarg);
			break;
		case 'd':
			dongle.dev_query = optarg;
			break;
		case 'f':
			if (controller.freq_len >= FREQUENCIES_LIMIT) {
				break;}
			if (strchr(optarg, ':'))
				{frequency_range(&controller, optarg);}
			else
			{
				controller.freqs[controller.freq_len] = (uint32_t)atofs(optarg);
				controller.freq_len++;
			}
			break;
		case 'g':
			dongle.gain_str = optarg;
			break;
		case 'l':
			demod.squelch_level = (int)atof(optarg);
			break;
		case 'L':
			printLevels = (int)atof(optarg);
			break;
		case 's':
			demod.rate_in = (uint32_t)atofs(optarg);
			demod.rate_out = (uint32_t)atofs(optarg);
			break;
		case 'r':
			output.rate = (int)atofs(optarg);
			demod.rate_out2 = (int)atofs(optarg);
			break;
		case 'o':
			fprintf(stderr, "Warning: -o is very buggy\n");
			demod.post_downsample = (int)atof(optarg);
			if (demod.post_downsample < 1 || demod.post_downsample > MAXIMUM_OVERSAMPLE) {
				fprintf(stderr, "Oversample must be between 1 and %i\n", MAXIMUM_OVERSAMPLE);}
			break;
		case 't':
			demod.conseq_squelch = (int)atof(optarg);
			if (demod.conseq_squelch < 0) {
				demod.conseq_squelch = -demod.conseq_squelch;
				demod.terminate_on_squelch = 1;
			}
			break;
		case 'p':
			dongle.ppm_error = atoi(optarg);
			custom_ppm = 1;
			break;
		case 'E':
			if (strcmp("edge",  optarg) == 0) {
				controller.edge = 1;}
			if (strcmp("dc", optarg) == 0 || strcmp("adc", optarg) == 0) {
				demod.dc_block_audio = 1;}
			if (strcmp("rdc", optarg) == 0) {
				demod.dc_block_raw = 1;}
			if (strcmp("deemp",  optarg) == 0) {
				demod.deemph = 1;}
			if (strcmp("direct",  optarg) == 0) {
				dongle.direct_sampling = 1;}
			if (strcmp("no-mod",  optarg) == 0) {
				dongle.direct_sampling = 3;}
			if (strcmp("offset",  optarg) == 0) {
				dongle.offset_tuning = 1;}
			if (strcmp("rtlagc", optarg) == 0 || strcmp("agc", optarg) == 0) {
				rtlagc = 1;}
			if (strcmp("zero", optarg) == 0) {
				demod.squelch_zero = 1;}
			if (strcmp("wav",  optarg) == 0) {
				output.wav_format = 1;}
			break;
		case 'q':
			demod.rdc_block_const = atoi(optarg);
			break;
		case 'F':
			demod.downsample_passes = 1;  /* truthy placeholder */
			demod.comp_fir_size = atoi(optarg);
			break;
		case 'A':
			if (strcmp("std",  optarg) == 0) {
				demod.custom_atan = 0;}
			if (strcmp("fast", optarg) == 0) {
				demod.custom_atan = 1;}
			if (strcmp("lut",  optarg) == 0) {
				atan_lut_init();
				demod.custom_atan = 2;}
			if (strcmp("ale", optarg) == 0) {
				demod.custom_atan = 3;}
			break;
		case 'M':
			if (strcmp("nbfm",  optarg) == 0 || strcmp("nfm",  optarg) == 0 || strcmp("fm",  optarg) == 0) {
				demod.mode_demod = &fm_demod;}
			if (strcmp("raw",  optarg) == 0 || strcmp("iq",  optarg) == 0) {
				demod.mode_demod = &raw_demod;}
			if (strcmp("am",  optarg) == 0) {
				demod.mode_demod = &am_demod;}
			if (strcmp("usb", optarg) == 0) {
				demod.mode_demod = &usb_demod;}
			if (strcmp("lsb", optarg) == 0) {
				demod.mode_demod = &lsb_demod;}
			if (strcmp("wbfm",  optarg) == 0 || strcmp("wfm",  optarg) == 0) {
				controller.wb_mode = 1;
				demod.mode_demod = &fm_demod;
				demod.rate_in = 170000;
				demod.rate_out = 170000;
				demod.rate_out2 = 32000;
				output.rate = 32000;
				demod.custom_atan = 1;
				//demod.post_downsample = 4;
				demod.deemph = 1;
				demod.squelch_level = 0;}
			break;
		case 'c':
			if (strcmp("us",  optarg) == 0)
				timeConstant = 75;
			else if (strcmp("eu", optarg) == 0)
				timeConstant = 50;
			else
				timeConstant = (int)atof(optarg);
			break;
		case 'v':
			++verbosity;
			break;
		case 'w':
			dongle.bandwidth = (uint32_t)atofs(optarg);
			if (dongle.bandwidth)
				dongle.offset_tuning = 1;		/* automatically switch offset tuning, when using bandwidth filter */
			break;
		case 'h':
		case '?':
		default:
			usage();
			break;
		}
	}

	if (verbosity)
		fprintf(stderr, "verbosity set to %d\n", verbosity);

	/* quadruple sample_rate to limit to Δθ to ±π/2 */
	demod.rate_in *= demod.post_downsample;

	if (!output.rate) {
		output.rate = demod.rate_out;}

	sanity_checks();

	if (controller.freq_len > 1) {
		demod.terminate_on_squelch = 0;}

	if (argc <= optind) {
		output.filename = "-";
	} else {
		output.filename = argv[optind];
	}

	ACTUAL_BUF_LENGTH = lcm_post[demod.post_downsample] * DEFAULT_BUF_LENGTH;

	tmp_stdout = suppress_stdout_start();
	verbose_device_search(dongle.dev_query, &dongle.dev);
	if (!dongle.dev) {
		fprintf(stderr, "Failed to open sdr device matching '%s'.\n", dongle.dev_query);
		exit(1);
	}
	verbose_setup_stream(dongle.dev, &dongle.stream, &dongle.channel, 1, SOAPY_SDR_CS16);

#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);
	signal(SIGPIPE, SIG_IGN);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif

	if (demod.deemph) {
		double tc = (double)timeConstant * 1e-6;
		demod.deemph_a = (int)round(1.0/((1.0-exp(-1.0/(demod.rate_out * tc)))));
		if (verbosity)
			fprintf(stderr, "using wbfm deemphasis filter with time constant %d us\n", timeConstant );
	}

	/* Set the antenna */
	if (NULL != antenna_str) {
		r = verbose_antenna_str_set(dongle.dev, dongle.channel, antenna_str);
		if (r != 0) {
			fprintf(stderr, "Failed to set antenna");
		}
	}

	/* Set the tuner gain */
	if (dongle.gain_str == NULL) {
		verbose_auto_gain(dongle.dev, dongle.channel);
	} else {
		verbose_gain_str_set(dongle.dev, dongle.gain_str, dongle.channel);
	}

	SoapySDRDevice_setGainMode(dongle.dev, SOAPY_SDR_RX, dongle.channel, rtlagc);

	if (custom_ppm) verbose_ppm_set(dongle.dev, dongle.ppm_error, dongle.channel);

 	verbose_set_bandwidth(dongle.dev, dongle.bandwidth, dongle.channel);

	if (verbosity && dongle.bandwidth)
	{
		fprintf(stderr, "Supported bandwidth values in kHz:\n");
		size_t bw_count = 0;
		// TODO: well, this is deprecated by getBandwidthRange? SoapySDRRange
		double *bandwidths = SoapySDRDevice_listBandwidths(dongle.dev, SOAPY_SDR_RX, dongle.channel, &bw_count);
		for (size_t k = 0; k < bw_count; ++k) {
			fprintf(stderr, "%.1f ", bandwidths[k]);
		}
		fprintf(stderr,"\n");
	}

	if (strcmp(output.filename, "-") == 0) { /* Write samples to stdout */
		output.file = stdout;
#ifdef _WIN32
		_setmode(_fileno(output.file), _O_BINARY);
#endif
	} else {
		output.file = fopen(output.filename, "wb");
		if (!output.file) {
			fprintf(stderr, "Failed to open %s\n", output.filename);
			exit(1);
		}
	}

	//r = rtlsdr_set_testmode(dongle.dev, 1);

	/* Reset endpoint before we start reading from it (mandatory) */
	verbose_reset_buffer(dongle.dev);

	pthread_create(&controller.thread, NULL, controller_thread_fn, (void *)(&controller));
	usleep(100000);
	pthread_create(&output.thread, NULL, output_thread_fn, (void *)(&output));
	pthread_create(&demod.thread, NULL, demod_thread_fn, (void *)(&demod));
	pthread_create(&dongle.thread, NULL, dongle_thread_fn, (void *)(&dongle));

	while (!do_exit) {
		usleep(100000);
	}

	SoapySDRDevice_deactivateStream(dongle.dev, dongle.stream, 0, 0);
	pthread_join(dongle.thread, NULL);
	safe_cond_signal(&demod.ready, &demod.ready_m);
	pthread_join(demod.thread, NULL);
	safe_cond_signal(&output.ready, &output.ready_m);
	pthread_join(output.thread, NULL);
	safe_cond_signal(&controller.hop, &controller.hop_m);
	pthread_join(controller.thread, NULL);

	//dongle_cleanup(&dongle);
	demod_cleanup(&demod);
	output_cleanup(&output);
	controller_cleanup(&controller);

	if (output.file != stdout) {
		fclose(output.file);}

	SoapySDRDevice_closeStream(dongle.dev, dongle.stream);
	SoapySDRDevice_unmake(dongle.dev);
	return EXIT_SUCCESS;
}

// vim: tabstop=8:softtabstop=8:shiftwidth=8:noexpandtab
