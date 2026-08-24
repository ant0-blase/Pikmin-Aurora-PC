#include "DebugLog.h"
#include "Dolphin/os.h"
#include "P2D/Graph.h"
#include "P2D/Picture.h"
#include "P2D/Screen.h"
#include "P2D/TextBox.h"
#include "P2D/Window.h"
#include "sysNew.h"
#include "zen/ogSub.h"

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(37)

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F4
 */
DEFINE_PRINT("P2DScreen")

/**
 * @todo: Documentation
 */
void P2DScreen::update()
{
	P2DPane::update();
	if (mAlphaMgr) {
		mAlphaMgr->update();
	}
	if (mTexAnimMgr) {
		mTexAnimMgr->update();
	}
}

/**
 * @todo: Documentation
 */
P2DScreen::~P2DScreen()
{
}

/**
 * @todo: Documentation
 */
void P2DScreen::set(const char* bloFileName, bool useAlphaMgr, bool useTexAnimMgr, bool p4)
{
	char path[PATH_MAX];
	makeResName(bloFileName, path);

#if defined(TARGET_PC)
	OSReport("[pikmin::p2d] set file=%s resolved=%s relative=%d screen=%p\n",
	         bloFileName ? bloFileName : "<null>", path, p4 ? 1 : 0, this);
#endif
	RandomAccessStream* file = gsys->openFile(path, p4);
	if (file) {
#if defined(TARGET_PC)
		OSReport("[pikmin::p2d] open ok stream=%p pos=%d pending=%d\n", file, file->getPosition(), file->getPending());
#endif
		makeHiearachyPanes(this, file, true, true);
#if defined(TARGET_PC)
		OSReport("[pikmin::p2d] hierarchy complete pos=%d pending=%d\n", file->getPosition(), file->getPending());
		OSReport("[pikmin::p2d] closing BLO stream=%p path=%s\n", file, path);
#endif
		file->close();
#if defined(TARGET_PC)
		OSReport("[pikmin::p2d] BLO stream closed path=%s\n", path);
#endif
	} else {
		PRINT("ERROR! Cannot open file.[%s] \n", path);
		ERROR("Cannot open file.[%s] \n", path);
	}

#if defined(TARGET_PC)
	OSReport("[pikmin::p2d-res] screen loadResource begin screen=%p path=%s\n", this, path);
#endif
	loadResource();
#if defined(TARGET_PC)
	OSReport("[pikmin::p2d-res] screen loadResource complete screen=%p path=%s\n", this, path);
#endif

	if (useAlphaMgr) {
		mAlphaMgr = new zen::PikaAlphaMgr(this);
		mAlphaMgr->start();
	} else {
		mAlphaMgr = nullptr;
	}

	if (useTexAnimMgr) {
		mTexAnimMgr = new zen::ogTexAnimMgr(this);
	} else {
		mTexAnimMgr = nullptr;
	}
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000030
 */
void P2DScreen::set(RandomAccessStream* input)
{
	makeHiearachyPanes(this, input, true, true);
}

/**
 * @todo: Documentation
 */
void P2DScreen::makeHiearachyPanes(P2DPane* parent, RandomAccessStream* input, bool, bool doExpandBounds)
{
#if defined(TARGET_PC)
	static int sDepth = 0;
	static u32 sRecord = 0;
	struct DepthGuard {
		int* depth;
		explicit DepthGuard(int* p) : depth(p) { ++*depth; }
		~DepthGuard() { --*depth; }
	} depthGuard(&sDepth);
	if (sDepth > 64) {
		OSPanic(__FILE__, __LINE__, "P2D hierarchy recursion exceeded 64 levels");
	}
#endif

	P2DPane* currPane = parent;
	while (true) {
#if defined(TARGET_PC)
		if (input->getPending() < 2) {
			OSReport("[pikmin::p2d] truncated hierarchy depth=%d pos=%d pending=%d parent=%p\n",
			         sDepth, input->getPosition(), input->getPending(), parent);
			OSPanic(__FILE__, __LINE__, "P2D hierarchy truncated");
		}
		const int recordPos = input->getPosition();
#endif
		u16 paneType = input->readShort();
#if defined(TARGET_PC)
		const u32 record = sRecord++;
		if (record < 96 || (record & 0x3f) == 0) {
			OSReport("[pikmin::p2d] rec=%u depth=%d pos=%d type=0x%04x pending=%d parent=%p curr=%p\n",
			         record, sDepth, recordPos, paneType, input->getPending(), parent, currPane);
		}
#endif

		switch (paneType) {
		case PANETYPE_Unk0:
		{
#if defined(TARGET_PC)
			OSReport("[pikmin::p2d] end depth=%d pos=%d\n", sDepth, input->getPosition());
#endif
			return;
		}
		case PANETYPE_Unk1:
		{
			u16 marker = input->readShort();
#if defined(TARGET_PC)
			OSReport("[pikmin::p2d] descend depth=%d marker=0x%04x curr=%p pos=%d\n",
			         sDepth, marker, currPane, input->getPosition());
#endif
			makeHiearachyPanes(currPane, input, true, false);
			break;
		}
		case PANETYPE_Unk2:
		{
			u16 marker = input->readShort();
#if defined(TARGET_PC)
			OSReport("[pikmin::p2d] ascend depth=%d marker=0x%04x pos=%d\n", sDepth, marker, input->getPosition());
#endif
			return;
		}
		case PANETYPE_Pane:
		{
#if defined(TARGET_PC)
			OSReport("[pikmin::p2d] construct PAN1 parent=%p pos=%d\n", parent, input->getPosition());
#endif
			currPane = new P2DPane(parent, input, paneType);
#if defined(TARGET_PC)
			OSReport("[pikmin::p2d] PAN1 ok pane=%p size=%dx%d pos=%d\n",
			         currPane, currPane->getWidth(), currPane->getHeight(), input->getPosition());
#endif
			if (doExpandBounds) {
				setBounds(PUTRect(0, 0, currPane->getWidth(), currPane->getHeight()));
			}
			break;
		}
		case PANETYPE_Window:
		{
#if defined(TARGET_PC)
			OSReport("[pikmin::p2d] construct WIN1 parent=%p pos=%d\n", parent, input->getPosition());
#endif
			currPane = new P2DWindow(parent, input, PANETYPE_Window);
#if defined(TARGET_PC)
			OSReport("[pikmin::p2d] WIN1 ok pane=%p pos=%d\n", currPane, input->getPosition());
#endif
			break;
		}
		case PANETYPE_Picture:
		{
#if defined(TARGET_PC)
			OSReport("[pikmin::p2d] construct PIC1 parent=%p pos=%d\n", parent, input->getPosition());
#endif
			currPane = new P2DPicture(parent, input, PANETYPE_Picture);
#if defined(TARGET_PC)
			OSReport("[pikmin::p2d] PIC1 ok pane=%p pos=%d\n", currPane, input->getPosition());
#endif
			break;
		}
		case PANETYPE_TextBox:
		{
#if defined(TARGET_PC)
			OSReport("[pikmin::p2d] construct TBX1 parent=%p pos=%d\n", parent, input->getPosition());
#endif
			currPane = new P2DTextBox(parent, input, PANETYPE_TextBox);
#if defined(TARGET_PC)
			OSReport("[pikmin::p2d] TBX1 ok pane=%p pos=%d\n", currPane, input->getPosition());
#endif
			break;
		}
#if defined(TARGET_PC)
		default:
		{
			OSReport("[pikmin::p2d] INVALID type=0x%04x depth=%d record=%u pos=%d pending=%d\n",
			         paneType, sDepth, record, recordPos, input->getPending());
			OSPanic(__FILE__, __LINE__, "Unknown P2D pane type");
			return;
		}
#endif
		}
	}
}

/**
 * @todo: Documentation
 */
P2DPane* P2DScreen::makeUserPane(u16, P2DPane*, RandomAccessStream*)
{
	ERROR("There is a unknown pane in SCRN resource\n");
	return nullptr;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000008
 */
P2DPane* P2DScreen::stop()
{
	ERROR("There is a unknown pane in SCRN resource\n");
	return nullptr;
}

/**
 * @todo: Documentation
 */
void P2DScreen::draw(int x, int y, const P2DGrafContext* grafContext)
{
	if (grafContext) {
		P2DGrafContext context(*grafContext);
		P2DPane::draw(x, y, grafContext, _EC);
		context.setScissor();
	} else {
		P2DOrthoGraph ortho(0, 0, 640, 480);
		ortho.setPort();
		P2DPane::draw(x, y, &ortho, _EC);
		ortho.setScissor();
	}

	GXSetNumTexGens(0);
	GXSetNumTevStages(1);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
	GXSetVtxDesc(GX_VA_TEX0, GX_NONE);
	GXSetCullMode(GX_CULL_NONE);
}

/**
 * @todo: Documentation
 */
P2DPane* P2DScreen::search(u32 tag, bool p2)
{
	if (!tag) {
		return nullptr;
	}

	return P2DPane::search(tag, p2);
}

/**
 * @todo: Documentation
 */
void P2DScreen::loadResource()
{
	loadChildResource();
}

/**
 * @todo: Documentation
 */
void P2DScreen::makeResName(const char* fileName, char* outPath)
{
	zen::makePathName(gsys->mBloDir, fileName, outPath);
	PRINT("makeResName:[%s] \n", outPath);
}
