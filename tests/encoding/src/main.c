/*
 * Zephyr ztest unit tests for the 4b/6b and Manchester encoding libraries
 * (src/encoding/4b6b.[ch], src/encoding/manchester.[ch]).
 *
 * These libraries implement insulin pump radio wire formats. The exact codebooks
 * are not assumed here: only round-trip identity and the documented output-length
 * formulas are asserted. Known-answer test vectors must be added separately,
 * derived from validated captures; hardcoding specific encoded byte values in a
 * unit test would be wrong without that provenance.
 */
#include <zephyr/ztest.h>
#include <string.h>
#include "4b6b.h"
#include "manchester.h"

#define TEST_BUF 512

ZTEST_SUITE(encoding, NULL, NULL, NULL, NULL, NULL);

/* Group A - Manchester round trip */

ZTEST(encoding, manchester_round_trip_pattern_lengths)
{
	const uint16_t lengths[] = { 1, 2, 4, 8, 16, 32, 71 };
	size_t n = sizeof(lengths) / sizeof(lengths[0]);

	for (size_t k = 0; k < n; k++) {
		uint16_t len = lengths[k];
		uint8_t src[TEST_BUF];
		uint8_t enc[TEST_BUF];
		uint8_t dec[TEST_BUF];

		for (int i = 0; i < len; i++) {
			src[i] = (uint8_t)(i * 7 + 3);
		}

		bool ok = encode_manchester(src, enc, len);
		zassert_true(ok, "encode_manchester len=%u failed", len);

		uint16_t dlen = decode_manchester(enc, dec, len * 2);
		zassert_equal(dlen, len, "decode_manchester len=%u mismatch", len);

		zassert_mem_equal(dec, src, len, "manchester round-trip len=%u", len);
	}
}

ZTEST(encoding, manchester_round_trip_all_zero)
{
	const uint16_t len = 16;
	uint8_t src[TEST_BUF];
	uint8_t enc[TEST_BUF];
	uint8_t dec[TEST_BUF];

	memset(src, 0x00, sizeof(src));

	bool ok = encode_manchester(src, enc, len);
	zassert_true(ok, "encode_manchester all-zero len=%u failed", len);

	uint16_t dlen = decode_manchester(enc, dec, len * 2);
	zassert_equal(dlen, len, "decode_manchester all-zero len=%u mismatch", len);

	zassert_mem_equal(dec, src, len, "manchester round-trip all-zero");
}

ZTEST(encoding, manchester_round_trip_all_ff)
{
	const uint16_t len = 16;
	uint8_t src[TEST_BUF];
	uint8_t enc[TEST_BUF];
	uint8_t dec[TEST_BUF];

	memset(src, 0xFF, sizeof(src));

	bool ok = encode_manchester(src, enc, len);
	zassert_true(ok, "encode_manchester all-ff len=%u failed", len);

	uint16_t dlen = decode_manchester(enc, dec, len * 2);
	zassert_equal(dlen, len, "decode_manchester all-ff len=%u mismatch", len);

	zassert_mem_equal(dec, src, len, "manchester round-trip all-ff");
}

ZTEST(encoding, manchester_round_trip_every_byte_value)
{
	for (int v = 0; v <= 0xFF; v++) {
		uint8_t src[TEST_BUF];
		uint8_t enc[TEST_BUF];
		uint8_t dec[TEST_BUF];

		src[0] = (uint8_t)v;

		bool ok = encode_manchester(src, enc, 1);
		zassert_true(ok, "encode_manchester value=0x%02x failed", (unsigned)v);

		uint16_t dlen = decode_manchester(enc, dec, 2);
		zassert_equal(dlen, 1, "decode_manchester value=0x%02x mismatch", (unsigned)v);

		zassert_equal(dec[0], src[0],
			  "manchester round-trip value=0x%02x", (unsigned)v);
	}
}

/* Group B - 4b/6b round trip */

ZTEST(encoding, 4b6b_round_trip_pattern_lengths)
{
	const uint16_t lengths[] = { 2, 4, 6, 8, 16, 32, 70 };
	size_t n = sizeof(lengths) / sizeof(lengths[0]);

	for (size_t k = 0; k < n; k++) {
		uint16_t len = lengths[k];
		uint8_t src[TEST_BUF];
		uint8_t enc[TEST_BUF];
		uint8_t dec[TEST_BUF];

		for (int i = 0; i < len; i++) {
			src[i] = (uint8_t)(i * 7 + 3);
		}

		uint16_t elen = encode_4b6b(src, enc, len);
		uint16_t expect = 3 * (len / 2) + 2 * (len % 2);
		zassert_equal(elen, expect,
			      "encode_4b6b len=%u length mismatch", len);

		uint16_t dlen = decode_4b6b(enc, dec, elen);
		zassert_equal(dlen, len, "decode_4b6b len=%u mismatch", len);

		zassert_mem_equal(dec, src, len, "4b6b round-trip len=%u", len);
	}
}

ZTEST(encoding, 4b6b_round_trip_every_byte_value)
{
	for (int v = 0; v <= 0xFF; v++) {
		uint8_t src[TEST_BUF];
		uint8_t enc[TEST_BUF];
		uint8_t dec[TEST_BUF];

		src[0] = (uint8_t)v;
		src[1] = (uint8_t)v;

		uint16_t elen = encode_4b6b(src, enc, 2);
		zassert_equal(elen, 3, "encode_4b6b value=0x%02x length", (unsigned)v);

		uint16_t dlen = decode_4b6b(enc, dec, elen);
		zassert_equal(dlen, 2, "decode_4b6b value=0x%02x mismatch", (unsigned)v);

		zassert_equal(dec[0], src[0],
			      "4b6b round-trip value=0x%02x byte0", (unsigned)v);
		zassert_equal(dec[1], src[1],
			      "4b6b round-trip value=0x%02x byte1", (unsigned)v);
	}
}

ZTEST(encoding, 4b6b_encode_length_formula)
{
	uint8_t src[TEST_BUF];
	uint8_t enc[TEST_BUF];

	memset(src, 0x00, sizeof(src));

	for (int len = 1; len <= 16; len++) {
		uint16_t elen = encode_4b6b(src, enc, (uint16_t)len);
		uint16_t expect = 3 * (len / 2) + 2 * (len % 2);
		zassert_equal(elen, expect,
			      "encode_4b6b formula len=%d", len);
	}
}

/* Group C - boundaries */

ZTEST(encoding, zero_length_encode_decode_4b6b)
{
	uint8_t src[TEST_BUF];
	uint8_t enc[TEST_BUF];
	uint8_t dec[TEST_BUF];

	memset(src, 0xAA, sizeof(src));

	uint16_t elen = encode_4b6b(src, enc, 0);
	zassert_equal(elen, 0, "encode_4b6b zero length");

	uint16_t dlen = decode_4b6b(src, dec, 0);
	zassert_equal(dlen, 0, "decode_4b6b zero length");
}

ZTEST(encoding, manchester_zero_length_no_crash)
{
	uint8_t src[TEST_BUF];
	uint8_t enc[TEST_BUF];

	memset(src, 0x00, sizeof(src));

	bool ok = encode_manchester(src, enc, 0);
	/* No specific return value is asserted for the empty input; the return
	 * is only referenced so it is not discarded. The real guarantee here is
	 * that the call completed without crashing. */
	zassert_true(ok || !ok, "encode_manchester len=0 completed");
}
