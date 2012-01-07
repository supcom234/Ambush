/////////////////////////////////////////////////////////////////////////////
////////////////////////////// API Hooker ///////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

#include <Windows.h>
#include <stdarg.h>
#include <map>
using namespace std;
#include "NCodeHook.cpp"
#include "signatures.h"
#include "slre.h"
#include "wslre.h"
#include "apihook.h"
#include "reporting.h"

////////////////////////////// Globals //////////////////////////////////////

//Our DLL-wide hooker, per-arch
#ifdef _M_X64
NCodeHook<ArchitectureX64> *hooker;
#else
NCodeHook<ArchitectureIA32> *hooker;
#endif

HMODULE myDllHandle; //my DLL base
map<void*, slre*> compiledSignatures; //for regex signatures
char* reportPipe;

//Function to get the original function and signature passed to hook0
hookArgFunc getHookArg = NULL;
//Helper function to return a value from a hook, used with stackFixups
OneArgFunc setEax = NULL;
//Helper function to get a pointer to the PEB
NoArgFunc getPEB = NULL;

//For allocating RWX memory,
HANDLE rwxHeap = NULL;
//For allocating RW memory
HANDLE rwHeap = NULL;
//Configuration
HOOKAPI_CONF * apiConf = NULL;
//Stack fixup code chunks for returning from STDCALL
#define NUM_STACK_FIXUPS 30
NoArgFunc stackFixups[NUM_STACK_FIXUPS];
//Special stack fixups for the loadlib calls
PBYTE popRet1arg, popRet3arg;
//Keeps track of how many and what DLLs we have already hooked
HMODULE* dllHandles = NULL;
unsigned int dllHandlesHooked = 0;
//Calls a generic STDAPI function and returns the result
ThreeArgFunc callApi = NULL;

////////////////////////////// Runtime Functions //////////////////////////////////////

//Does the memory at address match the given memory protection constraints?
inline bool memCompareProtect(void* address, unsigned int mode, unsigned int type){
	MEMORY_BASIC_INFORMATION mbi;
	if(VirtualQuery(address, &mbi, sizeof(mbi)) == 0)
		return true; //Error. default true
	switch(type){
	case ANY:
		return (mbi.Protect & mode) != 0;
	case ALL:
		return (mbi.Protect & mode) == mode;
	case ONE: //aka EXACT
		return mbi.Protect == mode;
	case NONE:
		return (mbi.Protect & mode) == 0;
	}
	return true;
}

//Evaluate each argument in a condition
inline bool argumentsMatch(HOOKAPI_ACTION_CONF* action, void** calledArgs){
	HOOKAPI_ARG_CONF* arg = actionConfArgs(action);
	for(unsigned int k = 0; k < action->numArgs; k++){
		void* calledArg = calledArgs[k];
		switch(arg->type){
		case DONTCARE:
			break;
		case DWORD_NEQ:
			if((size_t)(arg->value[0]) == (size_t)calledArg)
				return false;
			break;
		case DWORD_HOOK:
			if((size_t)(arg->value[0]) != (size_t)calledArg)
				return false;
			break;
		case DWORDRANGE:
			{
			bool belowLbound = (size_t)calledArg < (size_t)(arg->value[0]);
			bool aboveHbound = (size_t)calledArg > (size_t)(arg->value[1]);
			bool interiorRange = (size_t)(arg->value[0]) <= (size_t)(arg->value[1]);
			//e.g. range is 16 to 32
			if( ((belowLbound || aboveHbound) &&  interiorRange) ||
			//e.g. range is -16 to 16, interpreted here as Lbound 0xFFFFFFF0 Hbound 0x10 
			//so non-matches are < Lbound AND > Hbound
				((belowLbound && aboveHbound) && !interiorRange) )
				return false;
			break;
			}
		case CSTRING:
			{
				HOOKAPI_CSTRING_ARG* stringArg = (HOOKAPI_CSTRING_ARG*)arg->value;
				if(calledArg == NULL)
					return stringArg->value == 0; //Only empty strings match null
				if(compiledSignatures.count(arg) == 0){
					slre* regex = (slre*)HeapAlloc(rwHeap, HEAP_ZERO_MEMORY, sizeof(slre));
					slre_compile(regex, &stringArg->value);
					compiledSignatures[arg] = regex;
				}
				if(slre_match(compiledSignatures[arg], (char*)calledArg, lstrlenA((char*)calledArg), NULL) == 0)
					return false;
				break;
			}
		case WCSTRING:
			{
				HOOKAPI_WCSTRING_ARG* stringArg = (HOOKAPI_WCSTRING_ARG*)arg->value;
				if(calledArg == NULL)
					return stringArg->value == 0; //Only empty strings match null
				if(compiledSignatures.count(arg) == 0){
					wslre* regex = (wslre*)HeapAlloc(rwHeap, HEAP_ZERO_MEMORY, sizeof(wslre));
					wslre_compile(regex, &stringArg->value);
					compiledSignatures[arg] = (slre*)regex;
				}
				if(wslre_match((wslre*)compiledSignatures[arg], (wchar_t*)calledArg, lstrlenW((wchar_t*)calledArg), NULL) == 0)
					return false;
				break;
			}
		case MEM:
			{
				PHOOKAPI_MEM_ARG memArg = (PHOOKAPI_MEM_ARG)arg->value;
				if(memCompareProtect(calledArg, memArg->memMode, memArg->memType) != false)
					return false;
				break;
			}
		case BITMASK:
			{
				PHOOKAPI_BITMASK_ARG maskArg = (PHOOKAPI_BITMASK_ARG)arg->value;
				switch(maskArg->maskType){
				case ANY:
					if(((size_t)calledArg & maskArg->mask) == 0)
						return false;
					break;
				case ALL:
					if(((size_t)calledArg & maskArg->mask) != maskArg->mask)
						return false;
					break;
				case ONE: //aka EXACT
					if((size_t)calledArg != maskArg->mask)
						return false;
					break;
				case NONE:
					if(((size_t)calledArg & maskArg->mask) != 0)
						return false;
				}
			}
			break;
		case BLOB_HOOK:
			{
				HOOKAPI_BLOB_ARG* blobArg = (HOOKAPI_BLOB_ARG*)arg->value;
				if(calledArg == NULL)
					return blobArg->value == 0; //Only empty strings match null
				if(compiledSignatures.count(arg) == 0){
					slre* regex = (slre*)HeapAlloc(rwHeap, HEAP_ZERO_MEMORY, sizeof(slre));
					slre_compile(regex, &blobArg->value);
					compiledSignatures[arg] = regex;
				}
				size_t size = blobArg->size;
				if(blobArg->argument != -1)
					size = (size_t)calledArgs[blobArg->argument];
				if(size > INT_MAX)
					return true; // we're not even going to try
				if(slre_match(compiledSignatures[arg], (char*)calledArg, (int)size, NULL) != 1)
					return false;
			}
		}
		arg = nextArgConf(arg);
	}
	return true;
}

//Checks whether an action applies to this process
inline bool matchesProcess(HOOKAPI_ACTION_CONF* action){
	if(action->exePath[0] == '\0') // There is no condition, it matches
		return true;
	if(compiledSignatures.count(action) == 0){ // We haven't seen it before. Do the check.
		char exeFileName[MAX_PATH];
		DWORD exeNameLen = GetModuleFileNameA(NULL, exeFileName, sizeof(exeFileName));
		slre* regex = (slre*)HeapAlloc(rwHeap, HEAP_ZERO_MEMORY, sizeof(slre));
		slre_compile(regex, action->exePath);
		if(slre_match(regex, exeFileName, exeNameLen, NULL) == 1)
			compiledSignatures[action] = PROC_MATCH;
		else
			compiledSignatures[action] = NO_PROC_MATCH;
		HeapFree(rwHeap, 0, regex); // ok we're done
	}
	return compiledSignatures[action] == PROC_MATCH;
}

//This is the final hook that gets called for any function. It loads the function configuration
//and called arguments and takes any actions whose conditions match the corresponding signatures
//The 64 bit version has to initialize args so they are on the stack.
#ifdef _M_X64
void* hook0 (void* arg0, void* arg1, void* arg2, void* arg3){
	void** hookArgs = (*getHookArg)(); // opcode magic to get the hook arg pointer
	void** calledArgs = &arg0; // thanks to x64 convention, these will (sorta) be in order
	 //Ensure the other args actually written to the stack too
	calledArgs[1] = arg1;
	calledArgs[2] = arg2;
	calledArgs[3] = arg3;
#else
void* hook0 (void* arg0){
	void** hookArgs = (*getHookArg)(); // opcode magic to get the hook arg pointer
	void** calledArgs = &arg0; // thanks to STDCALL convention, these will be in order
#endif
	HOOKAPI_FUNC_CONF* conf = (HOOKAPI_FUNC_CONF*)hookArgs[0]; //get configuration
	NoArgFunc retfunc = stackFixups[conf->numArgs];

	//Evaluate each action - PRE
	HOOKAPI_ACTION_CONF* action = functionConfActions(conf);
	for(unsigned int i = 0; i < conf->numActions; i++){
		if(action->type == POST 	// Should we check now?
				|| matchesProcess(action) != true // Should check this process?
				// Does it match our return address constraints?
				|| ((action->retAddrMemType || action->retAddrMemMode) &&
				!memCompareProtect(calledArgs - 1, action->retAddrMemMode, 
				action->retAddrMemType))){
			//Then it's not our job. Continue
			action = nextActionConf(action);
			continue;
		}
		//Evaluate each argument
		if(argumentsMatch(action, calledArgs)){
			sendAlert(conf, action, calledArgs);
			//Take action!
			switch(action->action){
				case ALERT:
					break;
				case BLOCK:
					setEax((void*)action->retval);
					return retfunc();
				case KILLPROC:
					TerminateProcess(GetCurrentProcess(),ERROR_ACCESS_DENIED); //More forceful than ExitProcess()
				case KILLTHREAD:
					TerminateThread(GetCurrentThread(),ERROR_ACCESS_DENIED); //More forceful than ExitThread()
			}
		}
		//Next action
		action = nextActionConf(action);
	}

	//Now call the real function with a copy of the args.
	void* retval = callApi(&arg0, (void*)conf->numArgs, hookArgs[1]);

	//Evaluate each action - POST
	action = functionConfActions(conf);
	for(unsigned int i = 0; i < conf->numActions; i++){
		if(action->type == PRE 	// Should we check now?
				|| matchesProcess(action) != true // Should check this process?
				// Does it match our return address constraints?
				|| ((action->retAddrMemType || action->retAddrMemMode) &&
				!memCompareProtect(calledArgs - 1, action->retAddrMemMode, 
				action->retAddrMemType))){
			//Then it's not our job. Continue
			action = nextActionConf(action);
			continue;
		}
		//Evaluate each argument
		if(argumentsMatch(action, calledArgs)){
			sendAlert(conf, action, calledArgs);
			//Take action!
			switch(action->action){
				case ALERT:
					break;
				case BLOCK:
					setEax((void*)action->retval);
					return retfunc();
				case KILLPROC:
					TerminateProcess(GetCurrentProcess(),ERROR_ACCESS_DENIED); //More forceful than ExitProcess()
				case KILLTHREAD:
					TerminateThread(GetCurrentThread(),ERROR_ACCESS_DENIED); //More forceful than ExitThread()
			}
		}
		//Next action
		action = nextActionConf(action);
	}

	//And return the result
	setEax(retval);
	return retfunc();
}

//Hook for LoadLibraryA, LoadLibraryExA, LoadLibraryW, and LoadLibraryExW, to ensure new
//libraries are hooked. The hook argument passed must be an integer number of arguments
void* LoadLibraryHook(PVOID lpLibFileName, HANDLE hFile, PVOID dwFlags){
	void** hookArgs = (*getHookArg)(); // opcode magic to get the hook arg pointer
	HMODULE mod;
	NoArgFunc retfunc = (NoArgFunc)hookArgs[0];

	if(hookArgs[0] == popRet1arg){ // LoadLibrary
		mod = ((OneArgFunc)hookArgs[1])(lpLibFileName);
		GetLastError();
	}else if(hookArgs[0] == popRet3arg){// LoadLibraryEx
		mod = ((ThreeArgFunc)hookArgs[1])(lpLibFileName, hFile, dwFlags);
	}else{
		ExitProcess(ERROR_ACCESS_DENIED);//this shouldn't happen
	}
	if(mod != NULL)
		hookDllApi(mod); //We got a new library. Hook it if we need to.

	setEax(mod);
	//Now cleanup and return
	return retfunc();
}

////////////////////////////// Setup Functions //////////////////////////////////////

//Prepares some memory items we'll need before calling makeHook on an arbitrary function
//and loads configuration into memory
BOOL prepHookApi(){
	//get heaps and allocate dllHandles list
	rwHeap = GetProcessHeap();
	rwxHeap = HeapCreate(HEAP_CREATE_ENABLE_EXECUTE,0,0);
	//Our DLL-wide hooker, per-arch
	#ifdef _M_X64
	hooker = new NCodeHook<ArchitectureX64> (false,rwxHeap);
	#else
	hooker = new NCodeHook<ArchitectureIA32>(false,rwxHeap);
	#endif
	dllHandles = (HMODULE*)HeapAlloc(rwHeap, 0, 32 * sizeof(HMODULE));

	//Setup getHookArg()
	getHookArg = (hookArgFunc)HeapAlloc(rwxHeap, 0, sizeof(GET_HOOK_ARG));
	MoveMemory((PCHAR)getHookArg, GET_HOOK_ARG, sizeof(GET_HOOK_ARG)); 
	//Setup setEax()
	setEax = (OneArgFunc)HeapAlloc(rwxHeap, 0, sizeof(SET_EAX));
	MoveMemory((PCHAR)setEax, SET_EAX, sizeof(SET_EAX));
	//Setup getPEB()
	getPEB = (NoArgFunc)HeapAlloc(rwxHeap, 0, sizeof(GET_PEB));
	MoveMemory((PCHAR)getPEB, GET_PEB, sizeof(GET_PEB));
	//Setup callApi()
	callApi = (ThreeArgFunc)HeapAlloc(rwxHeap, 0, sizeof(CALL_API));
	MoveMemory((PCHAR)callApi, CALL_API, sizeof(CALL_API));

	//Setup stack fixups for returning
	for(WORD i = 0; i < NUM_STACK_FIXUPS; i++){
		stackFixups[i] = (NoArgFunc)HeapAlloc(rwxHeap, 0, sizeof(STACK_FIXUP) + 2);
		MoveMemory(stackFixups[i], STACK_FIXUP, sizeof(STACK_FIXUP));
		*((PWORD)((PBYTE)stackFixups[i] + sizeof(STACK_FIXUP) - 1)) = i * sizeof(void*); //
	}

	//Find configuration file from same binary directory as this file
	char filename[1000];
	DWORD size = GetModuleFileNameA(myDllHandle, filename, sizeof(filename));
	for(size -= 1; filename[size] != '\\' && size != 0; size--)
		filename[size] = 0;
	strcat_s(filename, "sig.dat"); // yes, I know this is VS-specific

	//Read configuration file
	HANDLE sigFileHandle = CreateFileA(filename,GENERIC_READ,
		FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,0);
	DWORD dontcare;
	DWORD fileSize = GetFileSize(sigFileHandle,NULL);
	if(fileSize == -1 || sigFileHandle == INVALID_HANDLE_VALUE ||
		(apiConf = (HOOKAPI_CONF*)HeapAlloc(rwHeap,0,fileSize)) == NULL ||
		ReadFile(sigFileHandle,apiConf,fileSize,&dontcare,NULL) == FALSE){
			CloseHandle(sigFileHandle);
			return FALSE;
	}
	CloseHandle(sigFileHandle);
	return TRUE;
}

//Makes a hook of an arbitrary function that calls hook0 with a given argument using a springboard
BOOL makeHook(void* origProcAddr, void* farg, void* hook){
	//args = [arg, origFunc]
	void** args = (void**)HeapAlloc(rwHeap, 0, sizeof(void*)*2);
	args[0] = farg;

	//Now make springboard
	HOOKAPI_SPRINGBOARD* springboard = getSpringboard(args, hook, rwxHeap);
	if(springboard == NULL)
		return FALSE;
	args[1] = hooker->createHook<NoArgFunc>((NoArgFunc)origProcAddr, (NoArgFunc)springboard);
	return TRUE;
}

//This function loads up hooks for a DLL if it has not been hooked before
BOOL hookDllApi(HMODULE dllHandle){
	//Check if valid
	if(dllHandle == NULL)
		return FALSE;

	//Check if we've seen it already
	for(size_t i = 0; i < dllHandlesHooked; i++)
		if(dllHandles[i] == dllHandle)
			return true;
	if(dllHandlesHooked != 0 && (dllHandlesHooked % 32) == 0){  //Out of mem
		void* newHandles = HeapAlloc(rwHeap, 0, (dllHandlesHooked + 32) * sizeof(HMODULE)); //get more
		MoveMemory(newHandles, dllHandles, dllHandlesHooked * sizeof(HMODULE)); // copy over
		HeapFree(rwHeap, 0, dllHandles); // release old
		dllHandles = (HMODULE*)newHandles; // tada!
	}
	dllHandles[dllHandlesHooked++] = dllHandle;

	//Get DLL name
	char filename[MAX_PATH+1];
	char name[MAX_PATH+1];
	GetModuleFileNameA(dllHandle,filename,sizeof(filename));
	int nameIndex = 0;
	for(int i = 0; i < sizeof(filename); i++){
		if(filename[i] == '\\'){
			nameIndex = i+1;
		}else{
			name[i - nameIndex] = filename[i];
			if(filename[i] >= 'A' && filename[i] <= 'Z')
				name[i - nameIndex] += 0x20; //convert to lower
			if(filename[i] == '\0')
				break;
		}
	}

	//Find DLL conf
	HOOKAPI_DLL_CONF* dllConf = apiConfDlls(apiConf);
	bool found = false;
	for(unsigned int i = 0; i < apiConf->numdlls; i++){
		if(strcmp(dllConf->name,name) == 0){
			found = true;
			break;
		}
		dllConf = nextDllConf(dllConf); // next
	}
	if(found == false)
		return FALSE;

	//Hook each function conf
	HOOKAPI_FUNC_CONF* function = dllConfFunctions(dllConf);
	for(unsigned int i = 0; i < dllConf->numFunctions; i++){
		makeHook(GetProcAddress(dllHandle, function->name), function, &hook0);
		function = nextFunctionConf(function); //next
	}
	return TRUE;
}

//Hook all loaded DLL's
bool hookAllDlls(){
	PPEB peb = (PPEB)getPEB(); // Get PEB
	//Walk module list and call hookDllApi for all
	PLIST_ENTRY moduleListHead = &peb->Ldr->InMemoryOrderModuleList;
	for(PLIST_ENTRY entry = moduleListHead->Flink;
		entry != moduleListHead; entry = entry->Flink)
		hookDllApi((HMODULE)((PLDR_DATA_TABLE_ENTRY)(entry - 1))->DllBase);
	return true;
}
//Post-DllMain-init. Doesn't really take an arg, just for CreateThread
DWORD WINAPI postInit(PVOID){
	hookAllDlls(); //Get any DLL's loaded after us
	return checkLogging(); //Setup a log server for this system or just check in
}

//And this is our main function
BOOL WINAPI DllMain(HINSTANCE instance, DWORD fdwReason, LPVOID){
	if(fdwReason != DLL_PROCESS_ATTACH)
		return TRUE;
	myDllHandle = instance;
	if(prepHookApi() == FALSE)
		return TRUE; //not really, but we want to exit without raising alarms

	hookAllDlls(); //Hook the ones we have now
	//And check again after all dlls are initialized
	CreateThread(NULL, 0, &postInit, NULL, 0, NULL); //This will run after all DllMains

	//Hook LoadLibrary calls
	HMODULE colonel = LoadLibraryA("kernel32");
	//Setup LoadLibrary return functions to fix the function tails. Stupid compiler.
	popRet1arg = (PBYTE)HeapAlloc(rwxHeap, 0, sizeof(POP_RET_ONE));
	MoveMemory(popRet1arg, POP_RET_ONE, sizeof(POP_RET_ONE));
	makeHook(GetProcAddress(colonel, "LoadLibraryA"), popRet1arg, &LoadLibraryHook);
	makeHook(GetProcAddress(colonel, "LoadLibraryW"), popRet1arg, &LoadLibraryHook);
	popRet3arg = (PBYTE)HeapAlloc(rwxHeap, 0, sizeof(POP_RET_THREE));
	MoveMemory(popRet3arg, POP_RET_THREE, sizeof(POP_RET_THREE));
	makeHook(GetProcAddress(colonel, "LoadLibraryExA"), popRet3arg, &LoadLibraryHook);
	makeHook(GetProcAddress(colonel, "LoadLibraryExW"), popRet3arg, &LoadLibraryHook);

	return TRUE;
}