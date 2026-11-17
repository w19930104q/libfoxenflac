/*
 *  libfoxenflac -- Tiny FLAC Decoder Library
 *  Copyright (C) 2018-2025  Andreas Stöckel
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file test_flac_slicing.c
 *
 * Decodes the same FLAC stream in one shot and sliced at arbitrary -- in
 * particular frame-tail-straddling -- byte boundaries and asserts that
 *
 *   - the decoded sample stream is bit-exact identical,
 *   - the frame segmentation (number of frames, samples per frame) and the
 *     final decoder state are identical,
 *   - decoding the stream twice in a row yields identical results,
 *   - corruptions of the CRC-protected frame region are always detected,
 *   - the decoder operates within a bounded amount of stack memory,
 *
 * all while the decoder instance itself resides in static memory, i.e.
 * without any dynamic memory allocation in the decode path.
 *
 * Usage: test_flac_slicing <FLAC FILE> [FLAC FILE...]
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#define HAVE_RLIMIT_STACK_TEST 1
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <foxen-flac.h>

/******************************************************************************
 * Configuration                                                              *
 ******************************************************************************/

/**
 * Maximum stack size the decoder (including this test driver) may use in the
 * runtime stack usage test. The decoder itself needs far less than this (its
 * largest stack frame is well below 1 kiB and it does not recurse); the
 * margin accounts for the C runtime and this test driver.
 */
#define STACK_LIMIT_BYTES (64U * 1024U)

/**
 * Statically allocated decoder memory. The decoder itself never performs any
 * dynamic allocation; this test proves it by running it entirely out of
 * static buffers. The second region is used for an "exact fit" decoder
 * instance dimensioned precisely for the stream being decoded (the typical
 * embedded configuration).
 */
static uint8_t flac_mem[3U * 1024U * 1024U];
static uint8_t flac_mem_exact[3U * 1024U * 1024U];

/**
 * Statically allocated output buffer (samples).
 */
#define OUT_CAP 4096U
static int32_t out_buf[OUT_CAP];

/******************************************************************************
 * Test framework                                                             *
 ******************************************************************************/

static int n_failures = 0;
static const char *cur_file = NULL;

#define CHECK(cond)                                                          \
	do {                                                                     \
		if (!(cond)) {                                                       \
			fprintf(stderr, "[ERR] %s:%d: CHECK(%s) failed (file: %s)\n",    \
			        __FILE__, __LINE__, #cond, cur_file);                    \
			n_failures++;                                                  \
		}                                                                    \
	} while (0)

/******************************************************************************
 * Decode driver                                                              *
 ******************************************************************************/

/**
 * Records everything observable about a decode run.
 */
typedef struct {
	int32_t *smpls;        /**< decoded interleaved samples */
	size_t n_smpls;        /**< number of decoded samples */
	size_t cap_smpls;      /**< capacity of smpls */
	uint32_t *frame_smpls; /**< cumulative samples at each frame end */
	uint64_t *frame_offs;  /**< approx. input offset at each frame end */
	size_t n_frames;       /**< number of completely decoded frames */
	size_t cap_frames;     /**< capacity of frame_smpls/frame_offs */
	uint64_t meta_off;     /**< input offset at FLAC_END_OF_METADATA */
	uint64_t last_frame_off; /**< approx. offset after the last frame */
	uint64_t n_samples_meta; /**< STREAMINFO sample count (0 = unknown) */
	uint64_t n_channels_meta; /**< STREAMINFO channel count */
	int saw_err;           /**< decoder entered FLAC_ERR */
	fx_flac_state_t final_state; /**< state after the last process call */
} result_t;

static void result_reset(result_t *res) {
	res->n_smpls = 0;
	res->n_frames = 0;
	res->meta_off = 0;
	res->last_frame_off = 0;
	res->n_samples_meta = 0;
	res->n_channels_meta = 0;
	res->saw_err = 0;
	res->final_state = FLAC_INIT;
}

static void result_free(result_t *res) {
	free(res->smpls);
	free(res->frame_smpls);
	free(res->frame_offs);
	memset(res, 0, sizeof(*res));
}

static void result_push_sample(result_t *res, int32_t s) {
	if (res->n_smpls == res->cap_smpls) {
		res->cap_smpls = res->cap_smpls ? res->cap_smpls * 2U : 4096U;
		res->smpls = (int32_t *)realloc(res->smpls,
		                                res->cap_smpls * sizeof(*res->smpls));
		if (!res->smpls) {
			fprintf(stderr, "[ERR] out of memory\n");
			exit(2);
		}
	}
	res->smpls[res->n_smpls++] = s;
}

static void result_push_frame(result_t *res, uint32_t n_smpls, uint64_t off) {
	if (res->n_frames == res->cap_frames) {
		res->cap_frames = res->cap_frames ? res->cap_frames * 2U : 16U;
		res->frame_smpls = (uint32_t *)realloc(
		    res->frame_smpls, res->cap_frames * sizeof(*res->frame_smpls));
		res->frame_offs = (uint64_t *)realloc(
		    res->frame_offs, res->cap_frames * sizeof(*res->frame_offs));
		if (!res->frame_smpls || !res->frame_offs) {
			fprintf(stderr, "[ERR] out of memory\n");
			exit(2);
		}
	}
	res->frame_smpls[res->n_frames] = n_smpls;
	res->frame_offs[res->n_frames] = off;
	res->n_frames++;
}

/**
 * Decodes the given data, feeding it to the decoder in chunks of
 * chunks[(i) % n_chunks] bytes and draining the output in chunks of out_cap
 * samples. Records all observable results in res.
 */
static void run_decode(fx_flac_t *flac, const uint8_t *data, size_t len,
                       const size_t *chunks, size_t n_chunks, uint32_t out_cap,
                       result_t *res) {
	result_reset(res);
	fx_flac_reset(flac);

	size_t off = 0, ci = 0;
	unsigned int stall = 0;
	while (true) {
		/* Fetch the next input chunk */
		const size_t rem = len - off;
		const size_t chunk = chunks[ci++ % n_chunks];
		uint32_t in_len = (uint32_t)((rem < chunk) ? rem : chunk);
		uint32_t out_len = out_cap;

		/* Advance the decoder */
		const fx_flac_state_t st =
		    fx_flac_process(flac, data + off, &in_len, out_buf, &out_len);
		off += in_len;
		res->final_state = st;

		/* Record observable events */
		if (st == FLAC_END_OF_METADATA) {
			res->meta_off = off;
			res->n_samples_meta = (uint64_t)fx_flac_get_streaminfo(
			    flac, FLAC_KEY_N_SAMPLES);
			res->n_channels_meta = (uint64_t)fx_flac_get_streaminfo(
			    flac, FLAC_KEY_N_CHANNELS);
		}
		for (uint32_t i = 0; i < out_len; i++) {
			result_push_sample(res, out_buf[i]);
		}
		if (st == FLAC_END_OF_FRAME) {
			result_push_frame(res, (uint32_t)res->n_smpls, off);
			res->last_frame_off = off;
		}
		if (st == FLAC_ERR) {
			res->saw_err = 1;
			break;
		}

		/* Termination/stall detection: the decoder must make progress
		   whenever there is input left or output space available. */
		if (in_len == 0 && out_len == 0) {
			if (off >= len) {
				break; /* quiescent: all input consumed */
			}
			if (++stall > 4U) {
				fprintf(stderr,
				        "[ERR] %s: decoder stalled at offset %zu/%zu\n",
				        cur_file, off, len);
				n_failures++;
				break;
			}
		} else {
			stall = 0;
		}
	}
}

/******************************************************************************
 * Slice patterns                                                             *
 ******************************************************************************/

/**
 * Compares a decode run against the reference run; all observable results
 * must be bit-exact identical.
 */
static void compare_to_ref(const result_t *ref, const result_t *res,
                           const char *what) {
	if (res->n_smpls != ref->n_smpls ||
	    (res->n_smpls &&
	     memcmp(res->smpls, ref->smpls,
	            res->n_smpls * sizeof(*res->smpls)) != 0)) {
		fprintf(stderr,
		        "[ERR] %s: %s: sample stream differs (%zu vs %zu samples)\n",
		        cur_file, what, res->n_smpls, ref->n_smpls);
		n_failures++;
	}
	if (res->n_frames != ref->n_frames ||
	    (res->n_frames &&
	     memcmp(res->frame_smpls, ref->frame_smpls,
	            res->n_frames * sizeof(*res->frame_smpls)) != 0)) {
		fprintf(stderr, "[ERR] %s: %s: frame segmentation differs\n",
		        cur_file, what);
		n_failures++;
	}
	CHECK(res->saw_err == ref->saw_err);
	CHECK(res->final_state == ref->final_state);
}

/**
 * Runs one slice pattern (with two different output drain sizes) and compares
 * the result to the reference run.
 */
static void check_pattern(fx_flac_t *flac, const uint8_t *data, size_t len,
                          const size_t *chunks, size_t n_chunks,
                          const result_t *ref, const char *what,
                          result_t *scratch) {
	run_decode(flac, data, len, chunks, n_chunks, OUT_CAP, scratch);
	compare_to_ref(ref, scratch, what);
	run_decode(flac, data, len, chunks, n_chunks, 1U, scratch);
	compare_to_ref(ref, scratch, what);
}

static void test_uniform_slices(fx_flac_t *flac, const uint8_t *data,
                                size_t len, const result_t *ref,
                                result_t *scratch) {
	static const size_t sizes[] = {1,  2,  3,  5,  7,   8,   13,  16,
		                           31, 64, 97, 128, 256, 512, 1024};
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		char what[64];
		snprintf(what, sizeof(what), "uniform slice %zu", sizes[i]);
		check_pattern(flac, data, len, &sizes[i], 1U, ref, what, scratch);
	}
}

static void test_random_slices(fx_flac_t *flac, const uint8_t *data, size_t len,
                               const result_t *ref, result_t *scratch) {
	/* Deterministic xorshift32 PRNG */
	for (uint32_t seed = 1U; seed <= 3U; seed++) {
		size_t chunks[512];
		uint32_t x = 0x9E3779B9U ^ (seed * 0x85EBCA6BU);
		for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
			x ^= x << 13U;
			x ^= x >> 17U;
			x ^= x << 5U;
			chunks[i] = 1U + (x % 97U);
		}
		char what[64];
		snprintf(what, sizeof(what), "random slices seed %u", seed);
		check_pattern(flac, data, len, chunks,
		              sizeof(chunks) / sizeof(chunks[0]), ref, what, scratch);
	}
}

/**
 * Manually crafted slice pattern straddling the (approximately known) frame
 * boundaries: cuts the stream "delta" bytes relative to each recorded frame
 * end, so that frame tails (padding + CRC16) and frame headers are split
 * across input buffer boundaries.
 */
static void test_frame_straddling_slices(fx_flac_t *flac, const uint8_t *data,
                                         size_t len, const result_t *ref,
                                         result_t *scratch) {
	if (ref->n_frames == 0 || ref->saw_err) {
		return;
	}

	/* Cut at frame_end + delta for various deltas. The offsets recorded by
	   the reference run may overshoot the true frame end by up to eight
	   bytes (the bitstream reader buffers up to eight bytes), which the
	   choice of deltas accounts for. */
	static const int deltas[] = {-9, -5, -2, -1, 0, 1};
	for (size_t d = 0; d < sizeof(deltas) / sizeof(deltas[0]); d++) {
		size_t chunks[2 * 256];
		size_t n_chunks = 0;
		size_t pos = 0;
		for (size_t i = 0; i < ref->n_frames && n_chunks + 2 <=
		                         sizeof(chunks) / sizeof(chunks[0]); i++) {
			const int64_t cut = (int64_t)ref->frame_offs[i] + deltas[d];
			if (cut > (int64_t)pos && (uint64_t)cut < len) {
				chunks[n_chunks++] = (size_t)cut - pos;
				pos = (size_t)cut;
			}
		}
		if (pos < len && n_chunks < sizeof(chunks) / sizeof(chunks[0])) {
			chunks[n_chunks++] = len - pos;
		}
		if (n_chunks == 0) {
			continue;
		}
		char what[64];
		snprintf(what, sizeof(what), "frame straddle delta %d", deltas[d]);
		check_pattern(flac, data, len, chunks, n_chunks, ref, what, scratch);
	}
}

/******************************************************************************
 * CRC corruption test                                                        *
 ******************************************************************************/

/**
 * Flips all bits of single bytes inside the CRC-protected frame region and
 * asserts that the decoder never silently accepts the corrupted stream: it
 * must either enter the error state or produce a different sample stream.
 */
static void test_crc_corruption(fx_flac_t *flac, const uint8_t *data,
                                size_t len, const result_t *ref,
                                result_t *scratch) {
	if (ref->saw_err || ref->n_frames == 0 || ref->meta_off == 0 ||
	    ref->last_frame_off <= ref->meta_off) {
		return; /* nothing CRC-protected to corrupt */
	}

	uint8_t *mut = (uint8_t *)malloc(len);
	if (!mut) {
		fprintf(stderr, "[ERR] out of memory\n");
		exit(2);
	}

	/* Corrupt bytes spread over the protected region */
	const size_t begin = (size_t)ref->meta_off;
	const size_t end = (size_t)ref->last_frame_off;
	const size_t step = ((end - begin) / 32U) ? ((end - begin) / 32U) : 1U;
	const size_t one_shot[] = {0xFFFFFFFFU};
	size_t n_detected = 0, n_total = 0;
	for (size_t off = begin; off < end; off += step) {
		memcpy(mut, data, len);
		mut[off] ^= 0xFFU;
		run_decode(flac, mut, len, one_shot, 1U, OUT_CAP, scratch);
		const bool detected =
		    scratch->saw_err || scratch->n_smpls != ref->n_smpls ||
		    (scratch->n_smpls &&
		     memcmp(scratch->smpls, ref->smpls,
		            scratch->n_smpls * sizeof(*scratch->smpls)) != 0);
		if (detected) {
			n_detected++;
		} else {
			fprintf(stderr,
			        "[ERR] %s: corruption at offset %zu not detected\n",
			        cur_file, off);
		}
		n_total++;
	}
	CHECK(n_detected == n_total);
	free(mut);
}

/******************************************************************************
 * Consecutive decode test                                                    *
 ******************************************************************************/

/**
 * Decodes the stream twice in a row with the same decoder instance (reset in
 * between) and asserts that both runs produce identical results.
 */
static void test_consecutive_decodes(fx_flac_t *flac, const uint8_t *data,
                                     size_t len, const result_t *ref,
                                     result_t *scratch) {
	static const size_t one[] = {0xFFFFFFFFU};
	run_decode(flac, data, len, one, 1U, OUT_CAP, scratch);
	compare_to_ref(ref, scratch, "second consecutive decode");
	run_decode(flac, data, len, one, 1U, OUT_CAP, scratch);
	compare_to_ref(ref, scratch, "third consecutive decode");
}

/******************************************************************************
 * Stack usage test                                                           *
 ******************************************************************************/

#if HAVE_RLIMIT_STACK_TEST
/**
 * Decodes the entire stream (one-shot and byte-wise) in a child process with
 * a drastically reduced stack size limit. If the decoder used unbounded or
 * large amounts of stack, the child would crash with SIGSEGV/SIGBUS.
 */
static void test_stack_usage(const uint8_t *data, size_t len) {
	fflush(NULL);
	const pid_t pid = fork();
	if (pid == 0) {
		/* Child: limit the stack, then decode */
		struct rlimit rl;
		if (getrlimit(RLIMIT_STACK, &rl) == 0) {
			rl.rlim_cur = STACK_LIMIT_BYTES;
			if (rl.rlim_max < rl.rlim_cur) {
				rl.rlim_cur = rl.rlim_max;
			}
			setrlimit(RLIMIT_STACK, &rl);
		}
		fx_flac_t *flac = fx_flac_init(flac_mem, FLAC_MAX_BLOCK_SIZE,
		                               FLAC_MAX_CHANNEL_COUNT);
		result_t res = {0};
		static const size_t one[] = {0xFFFFFFFFU};
		static const size_t seven[] = {7U};
		run_decode(flac, data, len, one, 1U, OUT_CAP, &res);
		run_decode(flac, data, len, seven, 1U, 1U, &res);
		_exit(0);
	}
	int status = 0;
	CHECK(pid > 0);
	if (pid > 0) {
		CHECK(waitpid(pid, &status, 0) == pid);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
			fprintf(stderr,
			        "[ERR] %s: decoder crashed with a %u kiB stack limit\n",
			        cur_file, STACK_LIMIT_BYTES / 1024U);
		}
	}
}
#endif /* HAVE_RLIMIT_STACK_TEST */

/******************************************************************************
 * Main program                                                               *
 ******************************************************************************/

static uint8_t *read_file(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	const long l = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (l <= 0) {
		fclose(f);
		return NULL;
	}
	uint8_t *buf = (uint8_t *)malloc((size_t)l);
	if (!buf || fread(buf, 1, (size_t)l, f) != (size_t)l) {
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);
	*len = (size_t)l;
	return buf;
}

static void test_file(const char *path) {
	cur_file = path;

	/* Read the input file */
	size_t len = 0;
	uint8_t *data = read_file(path, &len);
	if (!data) {
		fprintf(stderr, "[ERR] cannot read \"%s\"\n", path);
		n_failures++;
		return;
	}

	/* The decoder instance must fit into the static memory region; the
	   decoder itself never allocates memory dynamically. */
	const uint32_t flac_size =
	    fx_flac_size(FLAC_MAX_BLOCK_SIZE, FLAC_MAX_CHANNEL_COUNT);
	CHECK(flac_size > 0U && flac_size <= sizeof(flac_mem));
	fx_flac_t *flac_big = fx_flac_init(flac_mem, FLAC_MAX_BLOCK_SIZE,
	                                   FLAC_MAX_CHANNEL_COUNT);
	CHECK(flac_big != NULL);

	/* Reference run: decode the entire stream in one shot */
	result_t ref = {0}, scratch = {0};
	static const size_t one[] = {0xFFFFFFFFU};
	run_decode(flac_big, data, len, one, 1U, OUT_CAP, &ref);

	/* If the stream declares a sample count, the decoder must never produce
	   more samples than declared (it may produce fewer for truncated
	   streams). A complete, clean decode implies that all frame CRC16
	   checksums were verified successfully. */
	if (!ref.saw_err && ref.n_frames > 0 && ref.n_samples_meta > 0) {
		CHECK(ref.n_smpls <=
		      (size_t)(ref.n_samples_meta * ref.n_channels_meta));
	}

	/* Create an "exact fit" decoder instance dimensioned precisely for this
	   stream (the typical embedded configuration). This exercises the
	   decoder with no spare channel buffers and no spare block capacity.
	   Fall back to the maximally dimensioned instance if the stream
	   parameters are outside the supported range. */
	fx_flac_t *flac = flac_big;
	const uint64_t max_bs =
	    (uint64_t)fx_flac_get_streaminfo(flac_big, FLAC_KEY_MAX_BLOCK_SIZE);
	if (max_bs > 0U && max_bs <= FLAC_MAX_BLOCK_SIZE &&
	    ref.n_channels_meta >= 1U &&
	    ref.n_channels_meta <= FLAC_MAX_CHANNEL_COUNT &&
	    fx_flac_size((uint16_t)max_bs, (uint8_t)ref.n_channels_meta) <=
	        sizeof(flac_mem_exact)) {
		fx_flac_t *fit = fx_flac_init(flac_mem_exact, (uint16_t)max_bs,
		                              (uint8_t)ref.n_channels_meta);
		if (fit) {
			flac = fit;
		}
	}

	/* Sliced decoding must be bit-exact identical to the reference */
	test_uniform_slices(flac, data, len, &ref, &scratch);
	test_random_slices(flac, data, len, &ref, &scratch);
	test_frame_straddling_slices(flac, data, len, &ref, &scratch);

	/* Decoding twice in a row must yield identical results */
	test_consecutive_decodes(flac, data, len, &ref, &scratch);

	/* Corruptions in the CRC-protected region must be detected */
	test_crc_corruption(flac, data, len, &ref, &scratch);

#if HAVE_RLIMIT_STACK_TEST
	/* Decoding must work with a severely limited stack */
	test_stack_usage(data, len);
#endif

	result_free(&ref);
	result_free(&scratch);
	free(data);
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <FLAC FILE> [FLAC FILE...]\n", argv[0]);
		return 1;
	}
	for (int i = 1; i < argc; i++) {
		const int failures_before = n_failures;
		test_file(argv[i]);
		fprintf(stderr, "[%s] %s\n", (n_failures == failures_before) ? "OK" : "FAIL",
		        argv[i]);
	}
	if (n_failures) {
		fprintf(stderr, "[ERR] %d check(s) failed\n", n_failures);
		return 1;
	}
	return 0;
}
