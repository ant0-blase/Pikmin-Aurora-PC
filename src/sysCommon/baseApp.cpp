#include "BaseApp.h"
#include "Age.h"
#include "AtxStream.h"
#include "DebugLog.h"
#include "system.h"

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(__LINE__) // Never used in the DLL

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F0
 */
DEFINE_PRINT("baseApp");

/**
 * @todo: Documentation
 */
BaseApp::BaseApp()
{
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] BaseApp ctor begin this=%p nodeMgr=%p\n", this, nodeMgr);
#endif
	mIsReadyToDraw = FALSE;
	mCommandStream = nullptr;
	mAgeServer     = nullptr;
	_28            = 1;

#if defined(TARGET_PC)
	OSReport("[pikmin::boot] BaseApp attaching to root=%p\n", nodeMgr ? nodeMgr->firstNode() : nullptr);
#endif
	nodeMgr->firstNode()->add(this);
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] BaseApp ctor complete\n");
#endif
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000008
 */
int BaseApp::idleupdate()
{

	bool hasUpdates = false;
#ifdef WIN32
	if (mCommandStream) {
		const int commandStatus = mCommandStream->checkCommands();

		if (commandStatus == -1) {
			mCommandStream = nullptr;
		} else if (commandStatus) {
			hasUpdates = true;
		}
	}

	if (mAgeServer) {
		const int serverStatus = mAgeServer->update();

		if (serverStatus == -1) {
			stopAgeServer();
			mAgeServer = nullptr;
		} else if (serverStatus) {
			hasUpdates = true;
		}
	}
#endif
	return hasUpdates;
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000004
 */
void BaseApp::startAgeServer()
{
#ifdef WIN32
	if (mAgeServer) {
		return;
	}

	PRINT("Atx - Wants to open Age service\n");
	AgeServer* newServer = new AgeServer();

	if (newServer->Open()) {
		mAgeServer = newServer;

		immut char* windowName = Name();
		mAgeServer->NewNodeWindow(windowName);
		read(*(RandomAccessStream*)mAgeServer);
		mAgeServer->Done();
	}
#endif
}

/**
 * @todo: Documentation
 * @note UNUSED Size: 000004
 */
void BaseApp::stopAgeServer()
{
#ifdef WIN32
	if (mAgeServer) {
		PRINT("Atx - Wants to close Age service\n");
		mAgeServer->mStream->writeInt(ATX_CMD_CLOSE);
		mAgeServer->mStream->flush();
		mAgeServer = nullptr;
	}
#endif
}

/**
 * @todo: Documentation
 */
void BaseApp::softReset()
{
	stopAgeServer();
	mChild = nullptr;
	mWindowNode.init("[Windows]");
	gsys->initSoftReset();
}

/**
 * @todo: Documentation
 */
BaseApp::~BaseApp()
{
	PRINT("default baseApp deconstructor\n");

	if (mCommandStream) {
		mCommandStream->mStream->writeInt(ATX_CMD_CLOSE);
		mCommandStream->mStream->flush();
	}

	stopAgeServer();
	nodeMgr->Del(this);
}
