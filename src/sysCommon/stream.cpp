#include "Stream.h"

#include "Common/String.h"
#include <string.h>
#include <stdint.h>

// operator new[] is used without this header being included.
#if defined(BUGFIX)
#include "sysNew.h"
#endif

/**
 * @todo: Documentation
 */
int Stream::readInt()
{
	uint32_t raw;
	read(&raw, sizeof(raw));
#if defined(TARGET_PC) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	raw = __builtin_bswap32(raw);
#endif
	int value;
	memcpy(&value, &raw, sizeof(value));
	return value;
}

/**
 * @todo: Documentation
 */
u8 Stream::readByte()
{
	u8 c;
	read(&c, sizeof(u8));
	return c;
}

/**
 * @todo: Documentation
 */
short Stream::readShort()
{
	uint16_t raw;
	read(&raw, sizeof(raw));
#if defined(TARGET_PC) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	raw = __builtin_bswap16(raw);
#endif
	short value;
	memcpy(&value, &raw, sizeof(value));
	return value;
}

/**
 * @todo: Documentation
 */
f32 Stream::readFloat()
{
	uint32_t raw;
	read(&raw, sizeof(raw));
#if defined(TARGET_PC) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	raw = __builtin_bswap32(raw);
#endif
	f32 value;
	memcpy(&value, &raw, sizeof(value));
	return value;
}

/**
 * @todo: Documentation
 */
char* Stream::readString()
{
	int size = readInt();

	char* str = new char[size + 1];
	read(str, size);
	str[size] = '\0';
	return str;
}

/**
 * @todo: Documentation
 */
void Stream::readString(char* dest, int size)
{
	String str(dest, size);
	readString(str);
}

/**
 * @todo: Documentation
 */
void Stream::readString(String& str)
{
	int size = readInt();
	if (str.mLength < size) {
		str.init(size);
	}

	read(str.mString, size);
	str.mString[size] = '\0';
}

/**
 * @todo: Documentation
 */
void Stream::writeInt(int i)
{
	uint32_t raw;
	memcpy(&raw, &i, sizeof(raw));
#if (defined(TARGET_PC) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) || defined(WIN32)
	raw = __builtin_bswap32(raw);
#endif
	write(&raw, sizeof(raw));
}

/**
 * @todo: Documentation
 */
void Stream::writeByte(u8 c)
{
	write(&c, sizeof(u8));
}

/**
 * @todo: Documentation
 */
void Stream::writeShort(short value)
{
	uint16_t raw;
	memcpy(&raw, &value, sizeof(raw));
#if (defined(TARGET_PC) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) || defined(WIN32)
	raw = __builtin_bswap16(raw);
#endif
	write(&raw, sizeof(raw));
}

/**
 * @todo: Documentation
 */
void Stream::writeFloat(f32 value)
{
	uint32_t raw;
	memcpy(&raw, &value, sizeof(raw));
#if (defined(TARGET_PC) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) || defined(WIN32)
	raw = __builtin_bswap32(raw);
#endif
	write(&raw, sizeof(raw));
}

/**
 * @todo: Documentation
 */
void Stream::writeString(immut char* str)
{
	// `String` can't decide if it wants to be owning or non-owning.
	String s(const_cast<char*>(str), 0);
	writeString(s);
}

/**
 * @todo: Documentation
 */
void Stream::writeString(immut String& s)
{
	s32 length = ALIGN_NEXT(s.getLength(), 4);
	writeInt(length);
	write(s.mString, s.getLength());

	char c = 0;
	for (s32 i = 0; i < length - s.getLength(); i++) {
		write(&c, 1);
	}
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000C4 (Matching by size)
 */
void Stream::print(immut char* fmt, ...)
{
	char dest[1024];
	va_list args;
	va_start(args, fmt);
	vsprintf(dest, fmt, args);
	va_end(args);
	if (strlen(dest)) {
		write(dest, strlen(dest));
	}
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000064 (Matching by size)
 */
void Stream::vPrintf(immut char* param_1, va_list args)
{
	char dest[1024];
	vsprintf(dest, param_1, args);
	if (strlen(dest) != 0) {
		write(dest, strlen(dest));
	}
}

/**
 * @todo: Documentation
 */
void Stream::read(void*, int)
{
}

/**
 * @todo: Documentation
 */
void Stream::write(immut void*, int)
{
}

/**
 * @todo: Documentation
 */
int Stream::getPending()
{
	return 0;
}

/**
 * @todo: Documentation
 */
int Stream::getAvailable()
{
	return 0;
}

/**
 * @todo: Documentation
 */
void Stream::close()
{
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 00006C
 */
void RandomAccessStream::writeTo(int position, immut void* buffer, int length)
{
	setPosition(position);
	write(buffer, length);
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 00006C (Matching by size)
 */
void RandomAccessStream::readFrom(int position, void* buffer, int length)
{
	setPosition(position);
	read(buffer, length);
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 00005C (Matching by size)
 */
void RandomAccessStream::writeIntTo(int position, int value)
{
	setPosition(position);
	writeInt(value);
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 00004C (Matching by size)
 */
int RandomAccessStream::readIntFrom(int position)
{
	setPosition(position);
	int value = readInt();
	return value;
}
