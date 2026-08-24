#include "Stream.h"

#include "DebugLog.h"
#include "Dolphin/os.h"
#include "sysNew.h"
#include "system.h"
#include <string.h>

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(__LINE__) // Never used in the DLL

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F4
 */
DEFINE_PRINT("streamBufferedInput");

/**
 * @todo: Documentation
 */
void BufferedInputStream::init(Stream* stream, u8* buffer, int bufferSize)
{
	mPath             = StdSystem::stringDup(stream->mPath);
	mBufferSize       = bufferSize;
	mBuffer           = buffer ? buffer : new (0x20) u8[mBufferSize];
	mPosition         = 0;
	mCurrentBufferPos = 0;
	mRemainingBytes   = 0;
	mStream           = stream;
	fillBuffer();
}

/**
 * @todo: Documentation
 */
BufferedInputStream::BufferedInputStream(Stream* stream, u8* buffer, int bufferSize)
{
	init(stream, buffer, bufferSize);
}

/**
 * @todo: Documentation
 */
void BufferedInputStream::fillBuffer()
{
	if (mRemainingBytes - mCurrentBufferPos != 0) {
		return;
	}

	mRemainingBytes = getPending();

	if (mRemainingBytes > mBufferSize) {
		mRemainingBytes = mBufferSize;
	}

	mStream->read(mBuffer, mRemainingBytes);
	mCurrentBufferPos = 0;
}

/**
 * @todo: Documentation
 */
void BufferedInputStream::read(void* input, int size)
{
	u8* buf = static_cast<u8*>(input);
	while (size != 0) {
		fillBuffer();
		int diff     = mRemainingBytes - mCurrentBufferPos;
#if defined(TARGET_PC)
		if (diff <= 0) {
			// GameCube DVD/ARAM reads are performed in 32-byte units. A few retail
			// assets (notably stages/practice/default.gen) intentionally read a
			// small terminator from the physical sector tail even though the logical
			// file length has already been consumed. The host stream previously
			// panicked here, while the original hardware returned the padded bytes.
			//
			// Emulate only that final sector padding. We still panic if code tries to
			// walk beyond the next 32-byte boundary, so genuine corrupt/truncated
			// reads remain visible instead of being silently zero-filled forever.
			const int alignedEnd = ALIGN_NEXT(mPosition, 32);
			const int padAvail   = alignedEnd - mPosition;
			if (padAvail > 0) {
				const int padSize = size < padAvail ? size : padAvail;
				OSReport("[pikmin::stream] sector-tail padding path=%s pos=%d bytes=%d request=%d\n",
				         mPath ? mPath : "<unknown>", mPosition, padSize, size);
				memset(buf, 0, padSize);
				buf += padSize;
				size -= padSize;
				mPosition += padSize;
				continue;
			}

			OSReport("[pikmin::stream] read past padded EOF path=%s pos=%d remaining-request=%d pending=%d\n",
			         mPath ? mPath : "<unknown>", mPosition, size, getPending());
			OSPanic(__FILE__, __LINE__, "BufferedInputStream read past padded EOF");
		}
#endif
		int copySize = size;
		if (copySize > diff) {
			copySize = diff;
		}
		memcpy(buf, mBuffer + mCurrentBufferPos, copySize);
		buf += copySize;
		size -= copySize;
		mCurrentBufferPos += copySize;
		mPosition += copySize;
	}
}
