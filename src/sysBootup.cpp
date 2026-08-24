#include "App.h"
#include "sysNew.h"
#include "system.h"

/**
 * @todo: Documentation
 */
#if defined(TARGET_PC)
int pikmin_game_main(int argc, char* argv[])
#else
TERNARY_BUILD_MATCHING(void, int) main(int argc, char* argv[])
#endif
{
	gsys->Initialise();
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] returned from System::Initialise\n");
	OSReport("[pikmin::boot] allocating NodeMgr\n");
#endif
	nodeMgr = new NodeMgr();
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] NodeMgr ready=%p\n", nodeMgr);
	OSReport("[pikmin::boot] allocating PlugPikiApp\n");
#endif
	PlugPikiApp* gameApp = new PlugPikiApp();
#if defined(TARGET_PC)
	OSReport("[pikmin::boot] PlugPikiApp ready=%p, entering System::run\n", gameApp);
#endif
	gsys->run(gameApp);

#if defined(TARGET_PC)
	return 0;
#else
	OSErrorLine(29, "End of demo");
#endif
}
