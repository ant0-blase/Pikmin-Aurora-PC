#include "ID32.h"

#include "Age.h"
#include "DebugLog.h"
#include "Stream.h"
#include "sysNew.h"

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(__LINE__) // Never used in the DLL

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F0
 */
DEFINE_PRINT("id32");

/**
 * @brief Default constructor.
 */
ID32::ID32()
{
	setID('none');
}

/**
 * @brief Constructs an ID32.
 * @param id Initial ID value.
 */
ID32::ID32(u32 id)
{
	setID(id);
}

/**
 * @brief Sets the ID.
 * @param id ID value to store.
 */
void ID32::setID(u32 id)
{
	mId = id;
	updateString();
}

/**
 * @brief Matches an ID with a wildcard byte.
 * @param id Value to compare.
 * @param wild Wildcard byte to treat as "match any".
 * @return True if all four bytes match (or are wildcards).
 */
bool ID32::match(u32 id, char wild) immut
{
	for (int i = 0; i < 4; i++) {
		int shift  = (3 - i) * 8;
		char other = static_cast<char>((id >> shift) & 0xFF);
		char self  = static_cast<char>((mId >> shift) & 0xFF);

		if (other != wild && other != self) {
			return false;
		}
	}

	return true;
}

/**
 * @brief Updates mId from mStringID.
 */
void ID32::updateID()
{
	// ID32 values are FourCCs: the first character is always the most
	// significant byte, regardless of host endianness.
	mId = (static_cast<u32>(static_cast<u8>(mStringID[0])) << 24)
	    | (static_cast<u32>(static_cast<u8>(mStringID[1])) << 16)
	    | (static_cast<u32>(static_cast<u8>(mStringID[2])) << 8)
	    | static_cast<u32>(static_cast<u8>(mStringID[3]));
}

/**
 * @brief Updates mStringID from mId.
 */
void ID32::updateString()
{
	mStringID[0] = static_cast<char>((mId >> 24) & 0xFF);
	mStringID[1] = static_cast<char>((mId >> 16) & 0xFF);
	mStringID[2] = static_cast<char>((mId >> 8) & 0xFF);
	mStringID[3] = static_cast<char>(mId & 0xFF);
	mStringID[4] = 0;
}

/**
 * @brief Assignment operator.
 * @param other ID value.
 *
 * @note UNUSED Size: 000030
 */
void ID32::operator=(u32 other)
{
	setID(other);
}

/**
 * @brief Equality operator.
 */
bool ID32::operator==(u32 other) immut
{
	return mId == other;
}

/**
 * @brief Inequality operator.
 */
bool ID32::operator!=(u32 other) immut
{
	return mId != other;
}

/**
 * @brief Writes mId as 4 bytes.
 * @param stream Stream to write to.
 */
void ID32::write(RandomAccessStream& stream) immut
{
	// The original on-disc ID32 format is byte-reversed relative to the
	// canonical FourCC integer used by the game (GameCube code writes
	// id[3], id[2], id[1], id[0]). Keep that file format explicitly rather
	// than depending on host byte order.
	stream.writeByte(mId & 0xFF);
	stream.writeByte((mId >> 8) & 0xFF);
	stream.writeByte((mId >> 16) & 0xFF);
	stream.writeByte((mId >> 24) & 0xFF);
}

/**
 * @brief Reads mId and updates mStringID.
 * @param stream Stream to read from.
 */
void ID32::read(RandomAccessStream& stream)
{
	// ID32 files store the least-significant FourCC byte first. Rebuild the
	// canonical integer explicitly so the in-memory value stays identical on
	// big- and little-endian hosts.
	u32 b0 = static_cast<u32>(stream.readByte());
	u32 b1 = static_cast<u32>(stream.readByte());
	u32 b2 = static_cast<u32>(stream.readByte());
	u32 b3 = static_cast<u32>(stream.readByte());
	mId    = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);

	updateString();
}

/**
 * @brief Prints the ID.
 */
void ID32::print() immut
{
	PRINT("id (%x) is %s\n", mId, mStringID);
}

/**
 * @brief Formats mId into a 4-char string.
 * @param buffer Output buffer (>= 5 bytes).
 */
void ID32::sprint(char* buffer) immut
{
	buffer[0] = (mId & 0xFF000000) >> 24;
	buffer[1] = (mId & 0xFF0000) >> 16;
	buffer[2] = (mId & 0xFF00) >> 8;
	buffer[3] = (mId & 0xFF);
	buffer[4] = 0;
}

#ifdef WIN32

// .dll exclusive function

void ID32::genAge(AgeServer& server, char* name)
{
	server.setOnChange(new Delegate<ID32>(this, ageChangeID));
	server.NewEditor(name, mStringID, 5);
	server.setOnChange((Delegate<ID32>*)nullptr);
}

#endif
