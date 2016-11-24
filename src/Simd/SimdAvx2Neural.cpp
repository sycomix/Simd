/*
* Simd Library (http://simd.sourceforge.net).
*
* Copyright (c) 2011-2016 Yermalayeu Ihar.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy 
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell 
* copies of the Software, and to permit persons to whom the Software is 
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in 
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/
#include "Simd/SimdMemory.h"
#include "Simd/SimdExtract.h"
#include "Simd/SimdStore.h"
#include "Simd/SimdStream.h"

namespace Simd
{
#ifdef SIMD_AVX2_ENABLE    
    namespace Avx2
    {
		template <bool inversion> __m128i Invert(__m128i value);

		template <> __m128i Invert<true>(__m128i value)
		{
			return _mm_sub_epi8(Sse2::K_INV_ZERO, value);
		}

		template <> __m128i Invert<false>(__m128i value)
		{
			return value;
		}

		template <bool inversion, bool align, bool stream> void Convert(const uint8_t * src, const __m256 &_1_255, float * dst)
		{
			__m128i _src = Invert<inversion>(_mm_loadl_epi64((__m128i*)src));
			Avx::Stream<align, stream>(dst, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_src)), _1_255));
		}

		template <bool inversion, bool align, bool stream> void NeuralConvert(const uint8_t * src, size_t stride, size_t width, size_t height, float * dst)
		{
			assert(width >= 8);
			if (align)
				assert(Aligned(dst) && Aligned(width));

			size_t alignedWidth = AlignLo(width, 8);
			__m256 _1_255 = _mm256_set1_ps(1.0f / 255.0f);

			for (size_t row = 0; row < height; ++row)
			{
				for (size_t col = 0; col < alignedWidth; col += 8)
					Convert<inversion, align, stream>(src + col, _1_255, dst + col);
				if(width != alignedWidth)
					Convert<inversion, false, stream>(src + width - 8, _1_255, dst + width - 8);
				src += stride;
				dst += width;
			}
            if (stream)
                _mm_mfence();
		}

        template <bool inversion> void NeuralConvert(const uint8_t * src, size_t stride, size_t width, size_t height, float * dst)
        {
            if (Aligned(src) && Aligned(stride) && Aligned(dst) && Aligned(width))
            {
                if (width*height*sizeof(float) >= STREAM_SIZE_MIN)
                    NeuralConvert<inversion, true, true>(src, stride, width, height, dst);
                else
                    NeuralConvert<inversion, true, false>(src, stride, width, height, dst);
            }
            else
                NeuralConvert<inversion, false, false>(src, stride, width, height, dst);
        }

		void NeuralConvert(const uint8_t * src, size_t stride, size_t width, size_t height, float * dst, int inversion)
		{
			if (inversion)
				NeuralConvert<true>(src, stride, width, height, dst);
			else
				NeuralConvert<false>(src, stride, width, height, dst);
		}

        template <size_t size> SIMD_INLINE void LoadWeights(const float * src, __m256 * dst)
        {
            for (size_t i = 0; i < size; ++i)
                dst[i] = _mm256_set1_ps(src[i]);
        }

        template <bool align> SIMD_INLINE __m256 Convolution5(const float * src, const __m256 * weights)
        {
            return _mm256_fmadd_ps(Avx::Load<align>(src), weights[0], _mm256_add_ps(
                _mm256_fmadd_ps(Avx::Load<false>(src + 1), weights[1], _mm256_mul_ps(Avx::Load<false>(src + 2), weights[2])),
                _mm256_fmadd_ps(Avx::Load<false>(src + 4), weights[4], _mm256_mul_ps(Avx::Load<false>(src + 3), weights[3]))));
        }

        template <bool align> SIMD_INLINE __m256 Convolution5x5(const float * src, size_t stride, const __m256 * weights)
        {
            return _mm256_add_ps(Convolution5<align>(src, weights), _mm256_add_ps(
                _mm256_add_ps(Convolution5<align>(src + stride, weights + 5), Convolution5<align>(src + 2 * stride, weights + 10)),
                _mm256_add_ps(Convolution5<align>(src + 3 * stride, weights + 15), Convolution5<align>(src + 4 * stride, weights + 20))));
        }

        template <bool align> void NeuralAddConvolution5x5(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            size_t alignedWidth = AlignLo(width, F);
            __m256 tailMask = RightNotZero(width - alignedWidth);
            __m256 _weights[25];
            LoadWeights<25>(weights, _weights);
            for (size_t row = 0; row < height; ++row)
            {
                for (size_t col = 0; col < alignedWidth; col += F)
                {
                    __m256 _dst = Avx::Load<align>(dst + col);
                    _dst = _mm256_add_ps(_dst, Convolution5x5<align>(src + col, srcStride, _weights));
                    Avx::Store<align>(dst + col, _dst);
                }
                if (width - alignedWidth)
                {
                    size_t col = width - F;
                    __m256 _dst = Avx::Load<false>(dst + col);
                    _dst = _mm256_add_ps(_dst, _mm256_and_ps(tailMask, Convolution5x5<false>(src + col, srcStride, _weights)));
                    Avx::Store<false>(dst + col, _dst);
                }
                src += srcStride;
                dst += dstStride;
            }
        }

        void NeuralAddConvolution5x5(const float * src, size_t srcStride, size_t width, size_t height, const float * weights, float * dst, size_t dstStride)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolution5x5<true>(src, srcStride, width, height, weights, dst, dstStride);
            else
                NeuralAddConvolution5x5<false>(src, srcStride, width, height, weights, dst, dstStride);
        }

        template <bool align> SIMD_INLINE void AddMultiplied3(const float * src, const __m256 & dst, __m256 * sums)
        {
            sums[0] = _mm256_fmadd_ps(dst, Avx::Load<align>(src + 0), sums[0]);
            sums[1] = _mm256_fmadd_ps(dst, Avx::Load<false>(src + 1), sums[1]);
            sums[2] = _mm256_fmadd_ps(dst, Avx::Load<false>(src + 2), sums[2]);
        }

        template <bool align> SIMD_INLINE void AddMultiplied3x3(const float * src, size_t stride, const __m256 & dst, __m256 * sums)
        {
            AddMultiplied3<align>(src + stride * 0, dst, sums + 0);
            AddMultiplied3<align>(src + stride * 1, dst, sums + 3);
            AddMultiplied3<align>(src + stride * 2, dst, sums + 6);
        }

        template <bool align> void NeuralAddConvolution3x3Sum(const float * src, size_t srcStride, const float * dst, size_t dstStride, size_t width, size_t height, float * sums)
        {
            size_t alignedWidth = Simd::AlignLo(width, F);
            __m256 tailMask = RightNotZero(width - alignedWidth);
            __m256 _sums[9];
            memset(_sums, 0, sizeof(_sums));
            for (size_t row = 0; row < height; ++row)
            {
                for (size_t col = 0; col < alignedWidth; col += F)
                {
                    __m256 _dst = Avx::Load<align>(dst + col);
                    AddMultiplied3x3<align>(src + col, srcStride, _dst, _sums);
                }
                if (alignedWidth < width)
                {
                    size_t col = width - F;
                    __m256 _dst = _mm256_and_ps(tailMask, Avx::Load<false>(dst + col));
                    AddMultiplied3x3<false>(src + col, srcStride, _dst, _sums);
                }
                src += srcStride;
                dst += dstStride;
            }
            Add8ExtractedSums(_sums, sums);
            sums[8] += Avx::ExtractSum(_sums[8]);
        }

        void NeuralAddConvolution3x3Sum(const float * src, size_t srcStride, const float * dst, size_t dstStride, size_t width, size_t height, float * sums)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolution3x3Sum<true>(src, srcStride, dst, dstStride, width, height, sums);
            else
                NeuralAddConvolution3x3Sum<false>(src, srcStride, dst, dstStride, width, height, sums);
        }

        template <bool align> SIMD_INLINE void AddMultiplied5(const float * src, const __m256 & dst, __m256 * sums)
        {
            sums[0] = _mm256_fmadd_ps(dst, Avx::Load<align>(src + 0), sums[0]);
            sums[1] = _mm256_fmadd_ps(dst, Avx::Load<false>(src + 1), sums[1]);
            sums[2] = _mm256_fmadd_ps(dst, Avx::Load<false>(src + 2), sums[2]);
            sums[3] = _mm256_fmadd_ps(dst, Avx::Load<false>(src + 3), sums[3]);
            sums[4] = _mm256_fmadd_ps(dst, Avx::Load<false>(src + 4), sums[4]);
        }

        template <bool align> SIMD_INLINE void AddMultiplied5x5(const float * src, size_t stride, const __m256 & dst, __m256 * sums)
        {
            AddMultiplied5<align>(src + stride * 0, dst, sums + 0);
            AddMultiplied5<align>(src + stride * 1, dst, sums + 5);
            AddMultiplied5<align>(src + stride * 2, dst, sums + 10);
            AddMultiplied5<align>(src + stride * 3, dst, sums + 15);
            AddMultiplied5<align>(src + stride * 4, dst, sums + 20);
        }

        template <bool align> void NeuralAddConvolution5x5Sum(const float * src, size_t srcStride, const float * dst, size_t dstStride, size_t width, size_t height, float * sums)
        {
            size_t alignedWidth = Simd::AlignLo(width, F);
            __m256 tailMask = RightNotZero(width - alignedWidth);
            __m256 _sums[25];
            memset(_sums, 0, sizeof(_sums));
            for (size_t row = 0; row < height; ++row)
            {
                for (size_t col = 0; col < alignedWidth; col += F)
                {
                    __m256 _dst = Avx::Load<align>(dst + col);
                    AddMultiplied5x5<align>(src + col, srcStride, _dst, _sums);
                }
                if (alignedWidth < width)
                {
                    size_t col = width - F;
                    __m256 _dst = _mm256_and_ps(tailMask, Avx::Load<false>(dst + col));
                    AddMultiplied5x5<false>(src + col, srcStride, _dst, _sums);
                }
                src += srcStride;
                dst += dstStride;
            }
            Add8ExtractedSums(_sums + 0, sums + 0);
            Add8ExtractedSums(_sums + 8, sums + 8);
            Add8ExtractedSums(_sums + 16, sums + 16);
            sums[24] += Avx::ExtractSum(_sums[24]);
        }

        void NeuralAddConvolution5x5Sum(const float * src, size_t srcStride, const float * dst, size_t dstStride, size_t width, size_t height, float * sums)
        {
            if (Aligned(src) && Aligned(srcStride, F) && Aligned(dst) && Aligned(dstStride, F))
                NeuralAddConvolution5x5Sum<true>(src, srcStride, dst, dstStride, width, height, sums);
            else
                NeuralAddConvolution5x5Sum<false>(src, srcStride, dst, dstStride, width, height, sums);
        }
    }
#endif// SIMD_AVX2_ENABLE
}
