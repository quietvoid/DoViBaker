#include "DoViBaker.h"
#include "cube.h"

#include <array>
#include <io.h>

//////////////////////////////
// Code
//////////////////////////////

template<int quarterResolutionEl>
DoViBaker<quarterResolutionEl>::DoViBaker(
	PClip _blChild, 
	PClip _elChild, 
	const char* rpuPath, 
	bool _blChromaSubSampled, 
	bool _elChromaSubSampled,
	std::vector<std::pair<uint16_t, std::string>>& _cubes,
	bool _qnd,
	bool _rgbProof,
	bool _nlqProof,
	bool _outYUV,
	IScriptEnvironment* env)
  : GenericVideoFilter(_blChild), elChild(_elChild), qnd(_qnd), outYUV(_outYUV), blClipChromaSubSampled(_blChromaSubSampled), elClipChromaSubSampled(_elChromaSubSampled)
{
	int bits_per_pixel = vi.BitsPerComponent();
	if (bits_per_pixel != DoViProcessor::containerBitDepth) {
		env->ThrowError("DoViBaker: Video must be 16bit");
	}
	if (!outYUV)
		vi.pixel_type = VideoInfo::CS_RGBP16;

	doviProc = new DoViProcessor(rpuPath, env);
	if (!doviProc->wasCreationSuccessful()) {
		env->ThrowError("DoViBaker: Cannot create object");
	}
	doviProc->setRgbProof(_rgbProof);
	doviProc->setNlqProof(_nlqProof);

	if (vi.num_frames != doviProc->getClipLength()) {
		env->ThrowError("DoViBaker: Clip length does not match length indicated by RPU file");
	}

  CPU_FLAG = env->GetCPUFlags();
	int lutMaxCpuCaps = INT_MAX;

	for (int i = 0; i < _cubes.size(); i++) {
		auto cube_path = _cubes[i].second;
		if (_access(cube_path.c_str(), 0)) {
			env->ThrowError((std::string("DoViBaker: cannot open cube file ")+cube_path).c_str());
		}
		timecube::Cube cube = timecube::read_cube_from_file(cube_path.c_str());
		luts.push_back(std::pair(_cubes[i].first, timecube::create_lut_impl(cube, lutMaxCpuCaps)));
	}
}

template<int quarterResolutionEl>
DoViBaker<quarterResolutionEl>::~DoViBaker()
{
	doviProc->~DoViProcessor();
}

template<int quarterResolutionEl>
template<int vertLen, int nD>
inline void DoViBaker<quarterResolutionEl>::upsampleVert(PVideoFrame& dst, const PVideoFrame& src, const int plane, const std::array<int, vertLen>& Dn0p, const upscaler_t evenUpscaler, const upscaler_t oddUpscaler, IScriptEnvironment* env)
{
	const int srcHeight = src->GetHeight(plane);
	const int srcWidth = src->GetRowSize(plane) / sizeof(uint16_t);
	const int srcPitch = src->GetPitch(plane) / sizeof(uint16_t);
	const uint16_t* srcPb = (const uint16_t*)src->GetReadPtr(plane);

	const int dstPitch = dst->GetPitch(plane) / sizeof(uint16_t);
	uint16_t* dstPeven = (uint16_t*)dst->GetWritePtr(plane);
	uint16_t* dstPodd = dstPeven + dstPitch;

	std::array<const uint16_t*, vertLen> srcP;
	std::array<uint16_t, vertLen> value;
	auto& srcP0 = srcP[nD];

	for (int h0 = 0; h0 < srcHeight; h0++) {
		for (int i = 0; i < nD; i++) {
			int factor = max(h0 + Dn0p[i], 0);
			srcP[i] = srcPb + factor * srcPitch;
		}
		srcP0 = srcPb + h0 * srcPitch;
		for (int i = nD + 1; i < vertLen; i++) {
			int factor = min(h0 + Dn0p[i], srcHeight - 1);
			srcP[i] = srcPb + factor * srcPitch;
		}

		for (int w = 0; w < srcWidth; w++) {
			for (int i = 0; i < vertLen; i++) {
				value[i] = srcP[i][w];
			}
			dstPeven[w] = evenUpscaler(&value[0], nD);
			dstPodd[w] = oddUpscaler(&value[0], nD);
		}

		dstPeven += 2 * dstPitch;
		dstPodd += 2 * dstPitch;
	}
}

template<int quarterResolutionEl>
template<int vertLen, int nD>
void DoViBaker<quarterResolutionEl>::upsampleHorz(PVideoFrame& dst, const PVideoFrame& src, const int plane, const std::array<int, vertLen>& Dn0p, const upscaler_t evenUpscaler, const upscaler_t oddUpscaler, IScriptEnvironment* env)
{
	const int srcHeight = src->GetHeight(plane);
	const int srcWidth = src->GetRowSize(plane) / sizeof(uint16_t);
	const int srcPitch = src->GetPitch(plane) / sizeof(uint16_t);
	const uint16_t* srcP = (const uint16_t*)src->GetReadPtr(plane);

	const int dstPitch = dst->GetPitch(plane) / sizeof(uint16_t);
	uint16_t* dstP = (uint16_t*)dst->GetWritePtr(plane);

	static const int pD = vertLen - nD - 1;
	std::array<uint16_t, vertLen> value;

	for (int h = 0; h < srcHeight; h++) {
		for (int w = nD; w < srcWidth - pD; w++) {
			dstP[2 * w] = evenUpscaler(&srcP[w - nD], nD);
			dstP[2 * w + 1] = oddUpscaler(&srcP[w - nD], nD);
		}
		for (int w = 0; w < nD; w++) {
			for (int i = 0; i < nD; i++) {
				int wd = max(w + Dn0p[i], 0);
				value[i] = srcP[wd];
			}
			std::copy_n(&srcP[w], pD + 1, &value[nD]);
			dstP[2 * w] = evenUpscaler(&value[0], nD);
			dstP[2 * w + 1] = oddUpscaler(&value[0], nD);
		}
		for (int w = srcWidth - pD; w < srcWidth; w++) {
			for (int i = nD + 1; i < Dn0p.size(); i++) {
				int wd = min(w + Dn0p[i], srcWidth - 1);
				value[i] = srcP[wd];
			}
			std::copy_n(&srcP[w - nD], nD + 1, &value[0]);
			dstP[2 * w] = evenUpscaler(&value[0], nD);
			dstP[2 * w + 1] = oddUpscaler(&value[0], nD);
		}
		srcP += srcPitch;
		dstP += dstPitch;
	}
}

/*
* these commented out functions use processor functions which were replaced, see DoViProcessor.h
template<int quarterResolutionEl>
void DoViBaker<quarterResolutionEl>::upsampleHorz(PVideoFrame& dst, const PVideoFrame& mez, int plane, IScriptEnvironment* env)
{
	const int srcHeight = mez->GetHeight(plane);
	const int srcWidth = mez->GetRowSize(plane) / sizeof(uint16_t);
	const int srcPitch = mez->GetPitch(plane) / sizeof(uint16_t);
	const uint16_t* srcP = (const uint16_t*)mez->GetReadPtr(plane);

	const int dstHeight = dst->GetHeight(plane);
	const int dstWidth = dst->GetRowSize(plane) / sizeof(uint16_t);
	const int dstPitch = dst->GetPitch(plane) / sizeof(uint16_t);
	uint16_t* dstP = (uint16_t*)dst->GetWritePtr(plane);

	static const std::array<int, 8> Dn0p{ -3,-2,-1, 0, 1, 2, 3, 4 };
	static const int nD = 3;
	static const int pD = Dn0p.size() - nD - 1;
	std::array<uint16_t, Dn0p.size()> value;
	
	for (int h = 0; h < srcHeight; h++) {
		for (int w = nD; w < srcWidth - pD; w++) {
			dstP[2 * w] = doviProc->upsampleHorzEven(&srcP[w - nD], nD);
			dstP[2 * w + 1] = doviProc->upsampleHorzOdd(&srcP[w - nD], nD);
		}
		for (int w = 0; w < nD; w++) {
			for (int i = 0; i < nD; i++) {
				int wd = max(w + Dn0p[i], 0);
				value[i] = srcP[wd];
			}
			std::copy_n(&srcP[w], pD + 1, &value[nD]);
			dstP[2 * w] = doviProc->upsampleHorzEven(&value[0], nD);
			dstP[2 * w + 1] = doviProc->upsampleHorzOdd(&value[0], nD);
		}
		for (int w = srcWidth - pD; w < srcWidth; w++) {
			for (int i = nD + 1; i < Dn0p.size(); i++) {
				int wd = min(w + Dn0p[i], srcWidth - 1);
				value[i] = srcP[wd];
			}
			std::copy_n(&srcP[w - nD], nD + 1, &value[0]);
			dstP[2 * w] = doviProc->upsampleHorzEven(&value[0], nD);
			dstP[2 * w + 1] = doviProc->upsampleHorzOdd(&value[0], nD);
		}
		srcP += srcPitch;
		dstP += dstPitch;
	}
}

template<int quarterResolutionEl>
void DoViBaker<quarterResolutionEl>::upscaleEl(PVideoFrame& dst, const PVideoFrame& src, VideoInfo dstVi, IScriptEnvironment* env)
{
	dstVi.width /= 2;
	PVideoFrame mez = env->NewVideoFrame(dstVi);

	upsampleVert<5,2>(mez, src, PLANAR_Y, { -2,-1, 0, 1, 2 }, &DoViProcessor::upsampleElYvertEven, &DoViProcessor::upsampleElYvertOdd, env);
	upsampleVert<3,1>(mez, src, PLANAR_U, { -1, 0, 1 }, &DoViProcessor::upsampleElUVvertEven, &DoViProcessor::upsampleElUVvertOdd, env);
	upsampleVert<3,1>(mez, src, PLANAR_V, { -1, 0, 1 }, &DoViProcessor::upsampleElUVvertEven, &DoViProcessor::upsampleElUVvertOdd, env);
	
	upsampleHorz(dst, mez, PLANAR_Y, env);
	upsampleHorz(dst, mez, PLANAR_U, env);
	upsampleHorz(dst, mez, PLANAR_V, env);
}

template<int quarterResolutionEl>
void DoViBaker<quarterResolutionEl>::upsampleElChroma(PVideoFrame& dst, const PVideoFrame& src, VideoInfo dstVi, IScriptEnvironment* env)
{
	dstVi.width /= 2;
	PVideoFrame mez = env->NewVideoFrame(dstVi);

	upsampleVert<3, 1>(mez, src, PLANAR_U, { -1, 0, 1 }, &DoViProcessor::upsampleElUVvertEven, &DoViProcessor::upsampleElUVvertOdd, env);
	upsampleVert<3, 1>(mez, src, PLANAR_V, { -1, 0, 1 }, &DoViProcessor::upsampleElUVvertEven, &DoViProcessor::upsampleElUVvertOdd, env);

	upsampleHorz(dst, mez, PLANAR_U, env);
	upsampleHorz(dst, mez, PLANAR_V, env);
}


template<int quarterResolutionEl>
void DoViBaker<quarterResolutionEl>::upsampleBlChroma(PVideoFrame& dst, const PVideoFrame& src, VideoInfo dstVi, IScriptEnvironment* env)
{
	dstVi.width /= 2;
	PVideoFrame mez = env->NewVideoFrame(dstVi);
	
	upsampleVert<7,3>(mez, src, PLANAR_U, { -3,-2,-1, 0, 1, 2, 3 }, &DoViProcessor::upsampleBlVertEven, &DoViProcessor::upsampleBlVertOdd, env);
	upsampleVert<7,3>(mez, src, PLANAR_V, { -3,-2,-1, 0, 1, 2, 3 }, &DoViProcessor::upsampleBlVertEven, &DoViProcessor::upsampleBlVertOdd, env);

	upsampleHorz(dst, mez, PLANAR_U, env);
	upsampleHorz(dst, mez, PLANAR_V, env);
}
*/

template<int quarterResolutionEl>
void DoViBaker<quarterResolutionEl>::upscaleEl(PVideoFrame& dst, const PVideoFrame& src, VideoInfo dstVi, IScriptEnvironment* env)
{
	dstVi.width /= 2;
	PVideoFrame mez = env->NewVideoFrame(dstVi);

	upsampleVert<5, 2>(mez, src, PLANAR_Y, { -2,-1, 0, 1, 2 }, &DoViProcessor::upsampleLumaEven, &DoViProcessor::upsampleLumaOdd, env);
	upsampleVert<4, 1>(mez, src, PLANAR_U, { -1, 0, 1, 2 }, &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd, env);
	upsampleVert<4, 1>(mez, src, PLANAR_V, { -1, 0, 1, 2 }, &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd, env);
	
	upsampleHorz<5, 2>(dst, mez, PLANAR_Y, { -2,-1, 0, 1, 2 }, &DoViProcessor::upsampleLumaEven, &DoViProcessor::upsampleLumaOdd, env);
	upsampleHorz<4, 1>(dst, mez, PLANAR_U, { -1, 0, 1, 2 }, &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd, env);
	upsampleHorz<4, 1>(dst, mez, PLANAR_V, { -1, 0, 1, 2 }, &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd, env);
}

template<int quarterResolutionEl>
void DoViBaker<quarterResolutionEl>::upsampleChroma(PVideoFrame& dst, const PVideoFrame& src, VideoInfo dstVi, IScriptEnvironment* env)
{
	dstVi.width /= 2;
	PVideoFrame mez = env->NewVideoFrame(dstVi);

	upsampleVert<4, 1>(mez, src, PLANAR_U, { -1, 0, 1, 2 }, &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd, env);
	upsampleVert<4, 1>(mez, src, PLANAR_V, { -1, 0, 1, 2 }, &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd, env);

	upsampleHorz<4, 1>(dst, mez, PLANAR_U, { -1, 0, 1, 2 }, &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd, env);
	upsampleHorz<4, 1>(dst, mez, PLANAR_V, { -1, 0, 1, 2 }, &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd, env);
}

template<int quarterResolutionEl>
template<int chromaSubsampling>
void DoViBaker<quarterResolutionEl>::applyDovi(PVideoFrame& dst, const PVideoFrame& blSrcY, const PVideoFrame& blSrcUV, const PVideoFrame& elSrcY, const PVideoFrame& elSrcUV, IScriptEnvironment* env) const {

	const int blSrcPitchY = blSrcY->GetPitch(PLANAR_Y) / sizeof(uint16_t);

	const int elSrcPitchY = elSrcY->GetPitch(PLANAR_Y) / sizeof(uint16_t);

	const int dstPitchY = dst->GetPitch(PLANAR_Y) / sizeof(uint16_t);

	std::array<const uint16_t*, chromaSubsampling + 1> blSrcYp;
	std::array<const uint16_t*, chromaSubsampling + 1> elSrcYp;
	std::array<uint16_t*, chromaSubsampling + 1> dstYp;

	blSrcYp[0] = (const uint16_t*)blSrcY->GetReadPtr(PLANAR_Y);
	elSrcYp[0] = (const uint16_t*)elSrcY->GetReadPtr(PLANAR_Y);
	dstYp[0] = (uint16_t*)dst->GetWritePtr(PLANAR_Y);
	if (chromaSubsampling) {
		blSrcYp[1] = blSrcYp[0] + blSrcPitchY;
		elSrcYp[1] = elSrcYp[0] + elSrcPitchY;
		dstYp[1] = dstYp[0] + dstPitchY;
	}

	const int blSrcHeightUV = blSrcUV->GetHeight(PLANAR_U);
	const int blSrcWidthUV = blSrcUV->GetRowSize(PLANAR_U) / sizeof(uint16_t);
	const int blSrcPitchUV = blSrcUV->GetPitch(PLANAR_U) / sizeof(uint16_t);

	const int elSrcPitchUV = elSrcUV->GetPitch(PLANAR_U) / sizeof(uint16_t);

	const int dstPitchUV = dst->GetPitch(PLANAR_U) / sizeof(uint16_t);

	const uint16_t* blSrcUp = (const uint16_t*)blSrcUV->GetReadPtr(PLANAR_U);
	const uint16_t* elSrcUp = (const uint16_t*)elSrcUV->GetReadPtr(PLANAR_U);
	uint16_t* dstUp = (uint16_t*)dst->GetWritePtr(PLANAR_U);

	const uint16_t* blSrcVp = (const uint16_t*)blSrcUV->GetReadPtr(PLANAR_V);
	const uint16_t* elSrcVp = (const uint16_t*)elSrcUV->GetReadPtr(PLANAR_V);
	uint16_t* dstVp = (uint16_t*)dst->GetWritePtr(PLANAR_V);

	for (int huv = 0; huv < blSrcHeightUV; huv++) {
		if (chromaSubsampling) {
			int wuv = 0;
			for (int j = 0; j < chromaSubsampling + 1; j++) {
				for (int i = 0; i < chromaSubsampling + 1; i++) {
					const int w = (chromaSubsampling + 1) * wuv + i;
					dstYp[j][w] = doviProc->processSampleY(blSrcYp[j][w], elSrcYp[j][w]);
				}
			}
			int mmrBlY1 = 3 * blSrcYp[0][2 * wuv] + blSrcYp[0][2 * wuv + 1] + 2;
			int mmrBlY2 = 3 * blSrcYp[1][2 * wuv] + blSrcYp[1][2 * wuv + 1] + 2;
			uint16_t mmrBlY = ((mmrBlY1 >> 2) + (mmrBlY2 >> 2) + 1) >> 1;

			dstUp[wuv] = doviProc->processSampleU(blSrcUp[wuv], elSrcUp[wuv], mmrBlY, blSrcUp[wuv], blSrcVp[wuv]);
			dstVp[wuv] = doviProc->processSampleV(blSrcVp[wuv], elSrcVp[wuv], mmrBlY, blSrcUp[wuv], blSrcVp[wuv]);
		}

		for (int wuv = chromaSubsampling; wuv < blSrcWidthUV - chromaSubsampling; wuv++) {
			for (int j = 0; j < chromaSubsampling + 1; j++) {
				for (int i = 0; i < chromaSubsampling + 1; i++) {
					const int w = (chromaSubsampling + 1) * wuv + i;
					dstYp[j][w] = doviProc->processSampleY(blSrcYp[j][w], elSrcYp[j][w]);
				}
			}
			uint16_t mmrBlY;
			if (chromaSubsampling) {
				int mmrBlY1 = blSrcYp[0][2 * wuv - 1] + 2 * blSrcYp[0][2 * wuv] + blSrcYp[0][2 * wuv + 1] + 2;
				int mmrBlY2 = blSrcYp[1][2 * wuv - 1] + 2 * blSrcYp[1][2 * wuv] + blSrcYp[1][2 * wuv + 1] + 2;
				mmrBlY = ((mmrBlY1 >> 2) + (mmrBlY2 >> 2) + 1) >> 1;
			}
			else {
				mmrBlY = blSrcYp[0][wuv];
			}
			dstUp[wuv] = doviProc->processSampleU(blSrcUp[wuv], elSrcUp[wuv], mmrBlY, blSrcUp[wuv], blSrcVp[wuv]);
			dstVp[wuv] = doviProc->processSampleV(blSrcVp[wuv], elSrcVp[wuv], mmrBlY, blSrcUp[wuv], blSrcVp[wuv]);
		}

		if (chromaSubsampling) {
			int wuv = blSrcWidthUV - chromaSubsampling;
			for (int j = 0; j < chromaSubsampling + 1; j++) {
				for (int i = 0; i < chromaSubsampling + 1; i++) {
					const int w = (chromaSubsampling + 1) * wuv + i;
					dstYp[j][w] = doviProc->processSampleY(blSrcYp[j][w], elSrcYp[j][w]);
				}
			}
			int mmrBlY1 = blSrcYp[0][2 * wuv - 1] + 3 * blSrcYp[0][2 * wuv] + 2;
			int mmrBlY2 = blSrcYp[1][2 * wuv - 1] + 3 * blSrcYp[1][2 * wuv] + 2;
			uint16_t mmrBlY = ((mmrBlY1 >> 2) + (mmrBlY2 >> 2) + 1) >> 1;

			dstUp[wuv] = doviProc->processSampleU(blSrcUp[wuv], elSrcUp[wuv], mmrBlY, blSrcUp[wuv], blSrcVp[wuv]);
			dstVp[wuv] = doviProc->processSampleV(blSrcVp[wuv], elSrcVp[wuv], mmrBlY, blSrcUp[wuv], blSrcVp[wuv]);
		}

		for (int i = 0; i < chromaSubsampling + 1; i++) {
			blSrcYp[i] += blSrcPitchY * (chromaSubsampling + 1);
			elSrcYp[i] += elSrcPitchY * (chromaSubsampling + 1);
			dstYp[i] += dstPitchY * (chromaSubsampling + 1);
		}
		blSrcUp += blSrcPitchUV;
		blSrcVp += blSrcPitchUV;
		elSrcUp += elSrcPitchUV;
		elSrcVp += elSrcPitchUV;
		dstUp += dstPitchUV;
		dstVp += dstPitchUV;
	}
}

template<int quarterResolutionEl>
template<int blChromaSubsampling, int elChromaSubsampling>
void DoViBaker<quarterResolutionEl>::doAllQuickAndDirty(PVideoFrame& dst, const PVideoFrame& blSrc, const PVideoFrame& elSrc, IScriptEnvironment* env) const {
	const int blSrcPitchY = blSrc->GetPitch(PLANAR_Y) / sizeof(uint16_t);

	const int elSrcPitchY = elSrc->GetPitch(PLANAR_Y) / sizeof(uint16_t);

	const int dstPitch = dst->GetPitch(PLANAR_R) / sizeof(uint16_t);

	const int blSrcPitchUV = blSrc->GetPitch(PLANAR_U) / sizeof(uint16_t);

	const int elSrcHeightUV = elSrc->GetHeight(PLANAR_U);
	const int elSrcWidthUV = elSrc->GetRowSize(PLANAR_U) / sizeof(uint16_t);
	const int elSrcPitchUV = elSrc->GetPitch(PLANAR_U) / sizeof(uint16_t);

	const int blYvsElUVshifts = elChromaSubsampling + quarterResolutionEl;
	std::array<const uint16_t*, (1 << blYvsElUVshifts)> blSrcYp;
	std::array<const uint16_t*, (1 << elChromaSubsampling)> elSrcYp;
	std::array<uint16_t*, (1 << blYvsElUVshifts)> dstRp;
	blSrcYp[0] = (const uint16_t*)blSrc->GetReadPtr(PLANAR_Y);
	elSrcYp[0] = (const uint16_t*)elSrc->GetReadPtr(PLANAR_Y);
	dstRp[0] = (uint16_t*)dst->GetWritePtr(PLANAR_R);

	const int blUVvsElUVshifts = max(quarterResolutionEl + elChromaSubsampling - blChromaSubsampling, 0);
	std::array<const uint16_t*, (1 << blUVvsElUVshifts)> blSrcUp;
	std::array<const uint16_t*, 1> elSrcUp;
	std::array<uint16_t*, (1 << blYvsElUVshifts)> dstGp;
	blSrcUp[0] = (const uint16_t*)blSrc->GetReadPtr(PLANAR_U);
	elSrcUp[0] = (const uint16_t*)elSrc->GetReadPtr(PLANAR_U);
	dstGp[0] = (uint16_t*)dst->GetWritePtr(PLANAR_G);

	std::array<const uint16_t*, (1 << blUVvsElUVshifts)> blSrcVp;
	std::array<const uint16_t*, 1> elSrcVp;
	std::array<uint16_t*, (1 << blYvsElUVshifts)> dstBp;
	blSrcVp[0] = (const uint16_t*)blSrc->GetReadPtr(PLANAR_V);
	elSrcVp[0] = (const uint16_t*)elSrc->GetReadPtr(PLANAR_V);
	dstBp[0] = (uint16_t*)dst->GetWritePtr(PLANAR_B);

	for (int i = 1; i < elSrcVp.size(); i++) {
		elSrcUp[i] = elSrcUp[i - 1] + elSrcPitchUV;
		elSrcVp[i] = elSrcVp[i - 1] + elSrcPitchUV;
	}
	for (int i = 1; i < elSrcYp.size(); i++) {
		elSrcYp[i] = elSrcYp[i - 1] + elSrcPitchY;
	}
	for (int i = 1; i < blSrcVp.size(); i++) {
		blSrcUp[i] = blSrcUp[i - 1] + blSrcPitchUV;
		blSrcVp[i] = blSrcVp[i - 1] + blSrcPitchUV;
	}
	for (int i = 1; i < dstBp.size(); i++) {
		blSrcYp[i] = blSrcYp[i - 1] + blSrcPitchY;
		dstRp[i] = dstRp[i - 1] + dstPitch;
		dstGp[i] = dstGp[i - 1] + dstPitch;
		dstBp[i] = dstBp[i - 1] + dstPitch;
	}

	for (int heluv = 0; heluv < elSrcHeightUV; heluv++) {
		for (int weluv = 0; weluv < elSrcWidthUV; weluv++) {

			const uint16_t& elu = elSrcUp[0][weluv];
			const uint16_t& elv = elSrcVp[0][weluv];

			for (int hDbluv = 0; hDbluv < (1 << blUVvsElUVshifts); hDbluv++) {
				for (int wDbluv = 0; wDbluv < (1 << blUVvsElUVshifts); wDbluv++) {

					int wbluv = (weluv << blUVvsElUVshifts) + wDbluv;
					const uint16_t& blu = blSrcUp[hDbluv][wbluv];
					const uint16_t& blv = blSrcVp[hDbluv][wbluv];

					int hDbluvy = hDbluv << blChromaSubsampling;
					int wbluvy = wbluv << blChromaSubsampling;
					const uint16_t& mmrbly = blSrcYp[hDbluvy][wbluvy];

					const uint16_t& u = doviProc->processSampleU(blu, elu, mmrbly, blu, blv);
					const uint16_t& v = doviProc->processSampleV(blv, elv, mmrbly, blu, blv);

					for (int hDbly = 0; hDbly < blChromaSubsampling + 1; hDbly++) {
						for (int wDbly = 0; wDbly < blChromaSubsampling + 1; wDbly++) {

							int hDDbly = hDbluvy + hDbly;
							int wbly = wbluvy + wDbly;
							const uint16_t& bly = blSrcYp[hDDbly][wbly];

							int hDely = hDDbly >> quarterResolutionEl;
							int wely = wbly >> quarterResolutionEl;
							const uint16_t& ely = elSrcYp[hDely][wely];

							const uint16_t& y = doviProc->processSampleY(bly, ely);
							doviProc->sample2rgb(dstRp[hDDbly][wbly], dstGp[hDDbly][wbly], dstBp[hDDbly][wbly], y, u, v);
						}
					}
				}
			}
		}

		for (int i = 0; i < elSrcVp.size(); i++) {
			elSrcVp[i] += elSrcPitchUV;
			elSrcUp[i] += elSrcPitchUV;
		}
		for (int i = 0; i < elSrcYp.size(); i++) {
			elSrcYp[i] += elSrcPitchY * elSrcYp.size();
		}
		for (int i = 0; i < blSrcVp.size(); i++) {
			blSrcVp[i] += blSrcPitchUV * blSrcVp.size();
			blSrcUp[i] += blSrcPitchUV * blSrcVp.size();
		}
		for (int i = 0; i < blSrcYp.size(); i++) {
			blSrcYp[i] += blSrcPitchY * blSrcYp.size();
			dstRp[i] += dstPitch * blSrcYp.size();
			dstGp[i] += dstPitch * blSrcYp.size();
			dstBp[i] += dstPitch * blSrcYp.size();
		}
	}
}

template<int quarterResolutionEl>
void DoViBaker<quarterResolutionEl>::convert2rgb(PVideoFrame& dst, const PVideoFrame& srcY, const PVideoFrame& srcUV) const
{
	const int srcPitchY = srcY->GetPitch(PLANAR_Y) / sizeof(uint16_t);

	const int dstPitch = dst->GetPitch(PLANAR_R) / sizeof(uint16_t);

	const uint16_t* srcYp = (const uint16_t*)srcY->GetReadPtr(PLANAR_Y);
	uint16_t* dstRp = (uint16_t*)dst->GetWritePtr(PLANAR_R);

	const int srcHeightUV = srcUV->GetHeight(PLANAR_U);
	const int srcWidthUV = srcUV->GetRowSize(PLANAR_U) / sizeof(uint16_t);
	const int srcPitchUV = srcUV->GetPitch(PLANAR_U) / sizeof(uint16_t);

	const uint16_t* srcUp = (const uint16_t*)srcUV->GetReadPtr(PLANAR_U);
	uint16_t* dstGp = (uint16_t*)dst->GetWritePtr(PLANAR_G);

	const uint16_t* srcVp = (const uint16_t*)srcUV->GetReadPtr(PLANAR_V);
	uint16_t* dstBp = (uint16_t*)dst->GetWritePtr(PLANAR_B);

	for (int huv = 0; huv < srcHeightUV; huv++) {
		for (int wuv = 0; wuv < srcWidthUV; wuv++) {
			doviProc->sample2rgb(dstRp[wuv], dstGp[wuv], dstBp[wuv], srcYp[wuv], srcUp[wuv], srcVp[wuv]);
		}

		srcYp += srcPitchY;
		srcUp += srcPitchUV;
		srcVp += srcPitchUV;

		dstRp += dstPitch;
		dstGp += dstPitch;
		dstBp += dstPitch;
	}
}

template<int quarterResolutionEl>
void DoViBaker<quarterResolutionEl>::applyLut(PVideoFrame& dst, const PVideoFrame& src) const
{
	unsigned int width = vi.width;
	unsigned int height = vi.height;

	std::unique_ptr<float, decltype(&_aligned_free)> tmp_buf{ nullptr, _aligned_free };
	unsigned aligned_width = width % 8 ? (width - width % 8) + 8 : width;

	const uint16_t* src_p[3];
	int src_stride[3];
	uint16_t* dst_p[3];
	int dst_stride[3];
	float* tmp[3] = { 0 };

	src_p[0] = (const uint16_t*)src->GetReadPtr(PLANAR_R);
	src_p[1] = (const uint16_t*)src->GetReadPtr(PLANAR_G);
	src_p[2] = (const uint16_t*)src->GetReadPtr(PLANAR_B);
	src_stride[0] = src->GetPitch(PLANAR_R) / sizeof(uint16_t);
	src_stride[1] = src->GetPitch(PLANAR_G) / sizeof(uint16_t);
	src_stride[2] = src->GetPitch(PLANAR_B) / sizeof(uint16_t);
	dst_p[0] = (uint16_t*)dst->GetWritePtr(PLANAR_R);
	dst_p[1] = (uint16_t*)dst->GetWritePtr(PLANAR_G);
	dst_p[2] = (uint16_t*)dst->GetWritePtr(PLANAR_B);
	dst_stride[0] = dst->GetPitch(PLANAR_R) / sizeof(uint16_t);
	dst_stride[1] = dst->GetPitch(PLANAR_G) / sizeof(uint16_t);
	dst_stride[2] = dst->GetPitch(PLANAR_B) / sizeof(uint16_t);

	tmp_buf.reset((float*)_aligned_malloc(aligned_width * 3 * sizeof(float), 32));
	if (!tmp_buf)
		throw std::bad_alloc{};

	tmp[0] = tmp_buf.get();
	tmp[1] = tmp_buf.get() + aligned_width;
	tmp[2] = tmp_buf.get() + aligned_width * 2;

	timecube::PixelFormat format;
	format.type = (timecube::PixelType)1;
	format.depth = DoViProcessor::containerBitDepth;
	format.fullrange = true;

	for (unsigned i = 0; i < height; ++i)
	{
		current_frame_lut->to_float((const void**)src_p, tmp, format, width);
		current_frame_lut->process(tmp, tmp, width);
		current_frame_lut->from_float(tmp, (void**)dst_p, format, width);

		for (unsigned p = 0; p < 3; ++p)
		{
			src_p[p] += src_stride[p];
			dst_p[p] += dst_stride[p];
		}
	}
}

template<int quarterResolutionEl>
PVideoFrame DoViBaker<quarterResolutionEl>::GetFrame(int n, IScriptEnvironment* env)
{
	PVideoFrame blSrc = child->GetFrame(n, env);
	PVideoFrame elSrc = elChild ? elChild->GetFrame(n, env) : blSrc;
	PVideoFrame dst;
	if (!outYUV) {
		dst = env->NewVideoFrameP(vi, &blSrc);
	}
	
	bool doviInitialized = doviProc->intializeFrame(n, env);
	if (!doviInitialized) {
		return dst;
	}
	
	if (!outYUV) {
		env->propSetInt(env->getFramePropsRW(dst), "_Matrix", 0, 0);      //output is RGB
		env->propSetInt(env->getFramePropsRW(dst), "_ColorRange", 0, 0);  //output is full range RGB
		env->propDeleteKey(env->getFramePropsRW(dst), "_ChromaLocation"); //RGB has no chroma location defined
		env->propSetInt(env->getFramePropsRW(dst), "_dovi_max_pq", doviProc->getMaxPq(), 0);
		env->propSetInt(env->getFramePropsRW(dst), "_dovi_max_content_light_level", doviProc->getMaxContentLightLevel(), 0);
	}

	bool skipLut = luts.size() == 0;
	if (!skipLut) {
		current_frame_lut = luts[luts.size() - 1].second.get();
		for (int i = 1; i < luts.size(); i++) {
			if (doviProc->getMaxContentLightLevel() <= luts[i].first) {
				current_frame_lut = luts[i - 1].second.get();
				break;
			}
		}
	}

	bool skipElProcessing = false;
	if (!elChild || !doviProc->isFEL() || doviProc->elProcessingDisabled()) {
		skipElProcessing = true;
		doviProc->forceDisableElProcessing();
	}
	if (doviProc->isFEL() && !elChild) {
		env->ThrowError("DoViBaker: Expecting EL clip");
		return dst;
	}

	if (qnd) {
		if (blClipChromaSubSampled && elClipChromaSubSampled)
			doAllQuickAndDirty<true,true>(dst, blSrc, elSrc, env);
		else if (blClipChromaSubSampled && !elClipChromaSubSampled)
			doAllQuickAndDirty<true,false>(dst, blSrc, elSrc, env);
		else if (!blClipChromaSubSampled && elClipChromaSubSampled)
			doAllQuickAndDirty<false,true>(dst, blSrc, elSrc, env);
		else if (!blClipChromaSubSampled && !elClipChromaSubSampled)
			doAllQuickAndDirty<false,false>(dst, blSrc, elSrc, env);
	}
	else {
		PVideoFrame blSrc444;
		PVideoFrame elSrc444;
		PVideoFrame& elSrcR = elSrc;
		bool frameChromaSubSampled = blClipChromaSubSampled;
		if (!skipElProcessing) {
			if (quarterResolutionEl) {
				PVideoFrame elUpSrc;
				if (!blClipChromaSubSampled && elClipChromaSubSampled) {
					VideoInfo vi420 = child->GetVideoInfo();
					vi420.pixel_type = VideoInfo::CS_YUV420P16;
					elUpSrc = env->NewVideoFrame(vi420);
					upscaleEl(elUpSrc, elSrc, vi420, env);
				}
				else if (blClipChromaSubSampled && !elClipChromaSubSampled) {
					VideoInfo vi444 = child->GetVideoInfo();
					vi444.pixel_type = VideoInfo::CS_YUV444P16;
					elUpSrc = env->NewVideoFrame(vi444);
					upscaleEl(elUpSrc, elSrc, vi444, env);
				}
				else {
					elUpSrc = env->NewVideoFrame(child->GetVideoInfo());
					upscaleEl(elUpSrc, elSrc, child->GetVideoInfo(), env);
				}
				elSrc = elUpSrc;
			}
			if (!blClipChromaSubSampled && elClipChromaSubSampled) {
				elSrc444 = env->NewVideoFrame(child->GetVideoInfo());
				upsampleChroma(elSrc444, elSrc, child->GetVideoInfo(), env);
				frameChromaSubSampled = false;
			}
			if (blClipChromaSubSampled && !elClipChromaSubSampled) {
				VideoInfo vi444 = child->GetVideoInfo();
				vi444.pixel_type = VideoInfo::CS_YUV444P16;
				blSrc444 = env->NewVideoFrame(vi444);
				upsampleChroma(blSrc444, blSrc, vi444, env);
				frameChromaSubSampled = false;
			}
		}
		else { elSrcR = blSrc; }

		PVideoFrame mez = [&]() {
			if (blClipChromaSubSampled && !elClipChromaSubSampled) {
				VideoInfo vi444 = child->GetVideoInfo();
				vi444.pixel_type = VideoInfo::CS_YUV444P16;
				return env->NewVideoFrame(vi444);
			}
			else {
				return env->NewVideoFrame(child->GetVideoInfo());
			}
		}();
		if (frameChromaSubSampled)
			applyDovi<true>(mez, blSrc, (!blSrc444) ? blSrc : blSrc444, elSrcR, (!elSrc444) ? elSrcR : elSrc444, env);
		else
			applyDovi<false>(mez, blSrc, (!blSrc444) ? blSrc : blSrc444, elSrcR, (!elSrc444) ? elSrcR : elSrc444, env);

		if (outYUV) {
			env->copyFrameProps(blSrc, mez);
			env->propSetInt(env->getFramePropsRW(mez), "_dovi_max_pq", doviProc->getMaxPq(), 0);
			env->propSetInt(env->getFramePropsRW(mez), "_dovi_max_content_light_level", doviProc->getMaxContentLightLevel(), 0);
			return mez;
		}

		PVideoFrame mez444;
		if (frameChromaSubSampled) {
			VideoInfo vi444 = child->GetVideoInfo();
			vi444.pixel_type = VideoInfo::CS_YUV444P16;
			mez444 = env->NewVideoFrame(vi444);
			upsampleChroma(mez444, mez, vi444, env);
		}
		convert2rgb(dst, mez, (!mez444)? mez : mez444);
	}
	if (!skipLut) {
		applyLut(dst, dst);
	}
	return dst;
}

// explicitly instantiate the template for the linker
template class DoViBaker<true>;
template class DoViBaker<false>;

/*PVideoFrame DoViBaker::GetFrame(int n, IScriptEnvironment* env) {
	PVideoFrame src = elChild->GetFrame(n, env);
	PVideoFrame dst = env->NewVideoFrame(vi);

	static const int srcChannel[3] = { PLANAR_Y, PLANAR_U, PLANAR_V };

	if (vi.IsPlanar()) {

		for (int i = 0; i < 3; i++) {

			const int src_pitch = src->GetPitch(srcChannel[i]);
			const int src_width = src->GetRowSize(srcChannel[i]);
			const int src_height = src->GetHeight(srcChannel[i]);
			const unsigned char* srcp = src->GetReadPtr(srcChannel[i]);

			const int dst_pitch = dst->GetPitch(srcChannel[i]);
			const int dst_width = dst->GetRowSize(srcChannel[i]);
			const int dst_height = dst->GetHeight(srcChannel[i]);
			unsigned char* dstp = dst->GetWritePtr(srcChannel[i]);

			for (int h = 0; h < src_height; h++) {
				for (int w = 0; w < src_width; w++) {
					*(dstp + w) = *(srcp + w);
				}
				srcp += src_pitch;
				dstp += dst_pitch;
			}
		}
		return dst;
	}
}*/