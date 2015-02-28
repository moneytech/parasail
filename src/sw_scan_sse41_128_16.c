/**
 * @file
 *
 * @author jeff.daily@pnnl.gov
 *
 * Copyright (c) 2014 Battelle Memorial Institute.
 *
 * All rights reserved. No warranty, explicit or implicit, provided.
 */
#include "config.h"

#include <stdint.h>
#include <stdlib.h>

#include <emmintrin.h>
#include <smmintrin.h>

#include "parasail.h"
#include "parasail_internal.h"
#include "parasail_internal_sse.h"
#include "blosum/blosum_map.h"

#define NEG_INF_16 (INT16_MIN/(int16_t)(2))
#define MAX(a,b) ((a)>(b)?(a):(b))

static inline __m128i lrotate16(__m128i a) {
    return _mm_or_si128(
            _mm_slli_si128(a, 2), _mm_srli_si128(a, 14));
}

#ifdef PARASAIL_TABLE
static inline void arr_store_si128(
        int *array,
        __m128i vH,
        int32_t t,
        int32_t seglen,
        int32_t d,
        int32_t dlen)
{
    array[(0*seglen+t)*dlen + d] = (int16_t)_mm_extract_epi16(vH, 0);
    array[(1*seglen+t)*dlen + d] = (int16_t)_mm_extract_epi16(vH, 1);
    array[(2*seglen+t)*dlen + d] = (int16_t)_mm_extract_epi16(vH, 2);
    array[(3*seglen+t)*dlen + d] = (int16_t)_mm_extract_epi16(vH, 3);
    array[(4*seglen+t)*dlen + d] = (int16_t)_mm_extract_epi16(vH, 4);
    array[(5*seglen+t)*dlen + d] = (int16_t)_mm_extract_epi16(vH, 5);
    array[(6*seglen+t)*dlen + d] = (int16_t)_mm_extract_epi16(vH, 6);
    array[(7*seglen+t)*dlen + d] = (int16_t)_mm_extract_epi16(vH, 7);
}
#endif

#ifdef PARASAIL_TABLE
#define FNAME sw_table_scan_sse41_128_16
#else
#define FNAME sw_scan_sse41_128_16
#endif

parasail_result_t* FNAME(
        const char * const restrict s1, const int s1Len,
        const char * const restrict s2, const int s2Len,
        const int open, const int gap, const int matrix[24][24])
{
    int32_t i = 0;
    int32_t j = 0;
    int32_t k = 0;
    const int32_t n = 24; /* number of amino acids in table */
    const int32_t segWidth = 8; /* number of values in vector unit */
    int32_t segNum = 0;
    const int32_t segLen = (s1Len + segWidth - 1) / segWidth;
    __m128i* const restrict pvP = parasail_memalign_m128i(16, n * segLen);
    __m128i* const restrict pvE = parasail_memalign_m128i(16, segLen);
    __m128i* const restrict pvHt= parasail_memalign_m128i(16, segLen);
    __m128i* const restrict pvH = parasail_memalign_m128i(16, segLen);
    __m128i vGapO = _mm_set1_epi16(open);
    __m128i vGapE = _mm_set1_epi16(gap);
    __m128i vNegInf = _mm_set1_epi16(NEG_INF_16);
    __m128i vZero = _mm_setzero_si128();
    int16_t score = NEG_INF_16;
    const int16_t segLenXgap = -segLen*gap;
    __m128i vSegLenXgap1 = _mm_set1_epi16((segLen-1)*gap);
    __m128i vSegLenXgap = _mm_set_epi16(
            NEG_INF_16, segLenXgap, segLenXgap, segLenXgap,
            segLenXgap, segLenXgap, segLenXgap, segLenXgap);
    __m128i segLenXgap_reset = _mm_set_epi16(
            NEG_INF_16, NEG_INF_16, NEG_INF_16, NEG_INF_16,
            NEG_INF_16, NEG_INF_16, NEG_INF_16, -segLen*gap);
    __m128i rotate = _mm_set_epi8(13,12,11,10,9,8,7,6,5,4,3,2,1,0,15,14);
    __m128i insert = _mm_cmpeq_epi16(_mm_setzero_si128(),
            _mm_set_epi16(0,0,0,0,0,0,0,1));
    __m128i insert_back = _mm_cmpeq_epi16(_mm_setzero_si128(),
            _mm_set_epi16(1,0,0,0,0,0,0,0));
#ifdef PARASAIL_TABLE
    parasail_result_t *result = parasail_result_new_table1(segLen*segWidth, s2Len);
#else
    parasail_result_t *result = parasail_result_new();
#endif
    __m128i vMaxH = _mm_set1_epi16(NEG_INF_16);

    /* Generate query profile.
     * Rearrange query sequence & calculate the weight of match/mismatch.
     * Don't alias. */
    {
        int32_t index = 0;
        for (k=0; k<n; ++k) {
            for (i=0; i<segLen; ++i) {
                __m128i_16_t t;
                j = i;
                for (segNum=0; segNum<segWidth; ++segNum) {
                    t.v[segNum] = j >= s1Len ? 0 : matrix[k][MAP_BLOSUM_[(unsigned char)s1[j]]];
                    j += segLen;
                }
                _mm_store_si128(&pvP[index], t.m);
                ++index;
            }
        }
    }

    /* initialize H and E */
    {
        int32_t index = 0;
        for (i=0; i<segLen; ++i) {
            __m128i_16_t h;
            __m128i_16_t e;
            for (segNum=0; segNum<segWidth; ++segNum) {
                h.v[segNum] = 0;
                e.v[segNum] = NEG_INF_16;
            }
            _mm_store_si128(&pvH[index], h.m);
            _mm_store_si128(&pvE[index], e.m);
            ++index;
        }
    }

    /* outer loop over database sequence */
    for (j=0; j<s2Len; ++j) {
        __m128i vE;
        __m128i vHt;
        __m128i vFt;
        __m128i vH;
        __m128i vHp;
        __m128i *pvW;
        __m128i vW;

        /* calculate E */
        /* calculate Ht */
        /* calculate Ft */
        vHp = _mm_slli_si128(_mm_load_si128(pvH+(segLen-1)), 2);
        pvW = pvP + MAP_BLOSUM_[(unsigned char)s2[j]]*segLen;
        vHt = vNegInf;
        vFt = vNegInf;
        for (i=0; i<segLen; ++i) {
            vH = _mm_load_si128(pvH+i);
            vE = _mm_load_si128(pvE+i);
            vW = _mm_load_si128(pvW+i);
            vE = _mm_max_epi16(
                    _mm_sub_epi16(vE, vGapE),
                    _mm_sub_epi16(vH, vGapO));
            vFt = _mm_max_epi16(
                    _mm_sub_epi16(vFt, vGapE),
                    vHt);
            vHt = _mm_max_epi16(
                    _mm_add_epi16(vHp, vW),
                    vE);
            _mm_store_si128(pvE+i, vE);
            _mm_store_si128(pvHt+i, vHt);
            vHp = vH;
        }

        /* adjust Ft before local prefix scan */
        vHt = _mm_slli_si128(vHt, 2);
        vFt = _mm_max_epi16(vFt,
                _mm_sub_epi16(vHt, vSegLenXgap1));
        /* local prefix scan */
#if 0
        /* THIS WAS SLIGHTLY SLOWER THAN THE OTHER */
        vFt = _mm_blendv_epi8(vNegInf, vFt, insert_back);
        for (i=0; i<segWidth-1; ++i) {
            __m128i vFtt = lrotate16(vFt);
            vFtt = _mm_add_epi16(vFtt, vSegLenXgap);
            vFt = _mm_max_epi16(vFt, vFtt);
        }
        vFt = lrotate16(vFt);
#else
        {
            __m128i vFt_save = vFt;
            __m128i segLenXgap = segLenXgap_reset;
            for (i=0; i<segWidth-1; ++i) {
                __m128i vFtt = _mm_slli_si128(vFt, 2);
                segLenXgap = _mm_shuffle_epi8(segLenXgap, rotate);
                vFtt = _mm_add_epi16(vFtt, segLenXgap);
                vFt = _mm_max_epi16(vFt, vFtt);
            }
            vFt = _mm_blendv_epi8(vFt_save, vFt, insert);
        }
        vFt = _mm_slli_si128(vFt, 2);
        vFt = _mm_insert_epi16(vFt, NEG_INF_16, 0);
#endif

        /* second Ft pass */
        /* calculate vH */
        for (i=0; i<segLen; ++i) {
            vFt = _mm_max_epi16(
                    _mm_sub_epi16(vFt, vGapE),
                    vHt);
            vHt = _mm_load_si128(pvHt+i);
            vH = _mm_max_epi16(
                    vHt,
                    _mm_sub_epi16(vFt, vGapO));
            vH = _mm_max_epi16(vH, vZero);
            _mm_store_si128(pvH+i, vH);
#ifdef PARASAIL_TABLE
            arr_store_si128(result->score_table, vH, i, segLen, j, s2Len);
#endif
            /* update max vector seen so far */
            {
                vMaxH = _mm_max_epi16(vH, vMaxH);
            }
        }
    }

    /* max in vec */
    for (j=0; j<segWidth; ++j) {
        int16_t value = (int16_t) _mm_extract_epi16(vMaxH, 7);
        if (value > score) {
            score = value;
        }
        vMaxH = _mm_slli_si128(vMaxH, 2);
    }

    result->score = score;

    parasail_free(pvH);
    parasail_free(pvHt);
    parasail_free(pvE);
    parasail_free(pvP);

    return result;
}
