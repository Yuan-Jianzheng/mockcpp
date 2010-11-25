/***
    mockcpp is a generic C/C++ mock framework.
    Copyright (C) <2010>  <Darwin Yuan: darwin.yuan@gmail.com>
	                      <Chen Guodong: sinojelly@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#include <string.h>
#include <mockcpp/PageAllocator.h>
#include <mockcpp/BlockAllocator.h>
#include <mockcpp/CodeModifier.h>
#include <mockcpp/ArchApiHook.h>
#include <mockcpp/AllocatorContainer.h>


MOCKCPP_NS_START

namespace {

/*   
		|   frame pointer   |<-- ebp
		|   local vars      |
		|   parameters      | para1, para2, ...
		|   ret addr		| call func_to_be_mocked(para1,para2, ...)

					|         func_to_be_mocked: jmp to thunk   
					V
	 |->|   frame pointer   | 
	 |  |   local vars      |
	 |  |   parameters      |   
	 |  |   ret addr        |   
	 |--|   frame pointer   | thunk begin
	    |   old addr        |   
		|   ret addr        | call hook(old_addr, unused, unused, para1, para2, ...)
		| new frame pointer | hook begin
		|-------------------| <- ebp
		|   local vars      |
		|					| 
		|					| 

    
    push ebp
    mov ebp, esp
    mov eax, [new_addr]
    mov ecx, [old_addr]     
    push ecx
    call eax
    leave 
    ret 

    note: leave equals to 
	mov  esp,ebp 
	pop  ebp

	hook function is:
    hook(unsigned int old_addr, void* unused1, void* unused2, ...)
*/

const unsigned char thunkCodeTemplate[]  =  
{ 
	0x55,       // push ebp
	0x8B, 0xEC, // mov ebp, esp
	0xB8, 0x00, 0x00, 0x00, 0x00, // mov eax, [new_addr]
	0xB9, 0x00, 0x00, 0x00, 0x00, // mov ecx, [old_addr]
	0x51,       // push ecx
	0xFF, 0xD0, // call eax
	0xC9,       // leave
	0xC3        // ret
};


/*   __stdcall

		|   frame pointer   |<-- ebp
		|   local vars      |
		|   parameters      | para1, para2, ...
		|   ret addr		| call func_to_be_mocked(para1,para2, ...)

					|         func_to_be_mocked: jmp to thunk   
					V
	 |->|   frame pointer   | 
	 |  |   local vars      |
	 |  |   parameters      |   
	 |  |   old addr        | thunk begin, pop ret addr, push old addr, push ret addr => make ret addr at stack top  
	 |--|   ret addr        | and then jmp hook(old_addr, para1, para2, ...)
		| new frame pointer | hook begin
		|-------------------| <- ebp
		|   local vars      |
		|					| 
		|					| 

    
    mov eax, [new_addr]
    mov ecx, [old_addr]     
    pop edx   ; pop ret addr
    push ecx
    push edx
    jmp eax

    hook function is:
    hook(unsigned int old_addr, ...)
*/


const unsigned char thunkCodeTemplateStdcall[]  =  
{ 
	0xB8, 0x00, 0x00, 0x00, 0x00, // mov eax, [new_addr]
	0xB9, 0x00, 0x00, 0x00, 0x00, // mov ecx, [old_addr]
    0x5A,           // pop edx
    0x51,           // push ecx
    0x52,           // push edx
	0xFF, 0xE0      // jmp eax
};


// E9 :  jmp near
const unsigned char jmpCodeTemplate[]  =  
{ 
	0xE9, 0x00, 0x00, 0x00, 0x00  // jmp thunk
};

struct ThunkAllocator 
{
    static void *alloc()
    {
        return allocatorContainer.alloc(maxThunkCodeSize);
    }
    
    static void free(void *p)
    {
        allocatorContainer.free(p);
    }

    static void initialize(PageAllocator *pageAllocator)
    {
        static bool firstRun = true;

        if (firstRun)
        {
            firstRun = false;
            allocatorContainer.initialize(maxThunkCodeSize, pageAllocator);
        }
    }
    
private:
    static AllocatorContainer allocatorContainer;
    #define max(a, b) ((a) > (b) ? (a) : (b))
    static const size_t maxThunkCodeSize = max(sizeof( thunkCodeTemplate ), sizeof( thunkCodeTemplateStdcall ));
};

AllocatorContainer ThunkAllocator::allocatorContainer;

}


struct ArchApiHookImpl
{
	void hook(ApiHook::Address pfnOld, ApiHook::Address pfnNew, bool isStdcall );

	ArchApiHookImpl(PageAllocator *pageAllocator, CodeModifier *codeModifier);
	~ArchApiHookImpl();

private:
	ApiHook::Address  m_pfnOld; // save old func addr.
	
    char m_byNew[sizeof(jmpCodeTemplate)];  //jmp to thunk
	char m_byOld[sizeof(jmpCodeTemplate)];  //save old func content which will be covered with jmp to thunk code, so as to recover it when unhook.
	char *m_thunk; //thunk code, for jumping to mock func(CApiHookFunctor<BOOST_TYPEOF(function)>::hook) and pass old func addr as parameter.

    PageAllocator *allocator;
	CodeModifier *modifier;

	bool changeCode(char* code);
	bool allocThunk();
	void freeThunk();
	void initHook(ApiHook::Address pfnOld, ApiHook::Address pfnNew );
	void initThunk(ApiHook::Address pfnOld, ApiHook::Address pfnNew, bool isStdcall);
	void startHook();
	void stopHook();
};

/////////////////////////////////////////////////////////////////
ArchApiHookImpl::ArchApiHookImpl(PageAllocator *pageAllocator, CodeModifier *codeModifier)
	: allocator(pageAllocator), modifier(codeModifier)
{
    ThunkAllocator::initialize(pageAllocator);
}

/////////////////////////////////////////////////////////////////
bool ArchApiHookImpl::allocThunk()
{
	m_thunk = (char *)ThunkAllocator::alloc();
	return m_thunk != 0;
}

/////////////////////////////////////////////////////////////////
void ArchApiHookImpl::freeThunk()
{
	ThunkAllocator::free(m_thunk);
}

/////////////////////////////////////////////////////////////////
void ArchApiHookImpl::initThunk(ApiHook::Address pfnOld, ApiHook::Address pfnNew, bool isStdcall)
{
    if (isStdcall)
    {
    	memcpy( m_thunk, thunkCodeTemplateStdcall, sizeof( thunkCodeTemplateStdcall ) );  
    	*(unsigned long*)(m_thunk + 1) = (unsigned long)pfnNew;
    	*(unsigned long*)(m_thunk + 6) = (unsigned long)pfnOld;
    }
    else
    {
    	memcpy( m_thunk, thunkCodeTemplate, sizeof( thunkCodeTemplate ) );  
    	*(unsigned long*)(m_thunk + 4) = (unsigned long)pfnNew;
    	*(unsigned long*)(m_thunk + 9) = (unsigned long)pfnOld;
    }
}

/////////////////////////////////////////////////////////////////
void ArchApiHookImpl::initHook(ApiHook::Address pfnOld, ApiHook::Address pfnNew )
{
    memcpy( m_byNew, jmpCodeTemplate, sizeof( jmpCodeTemplate ) );  

    *(unsigned long*)(m_byNew + 1) = 
		(unsigned long)m_thunk - (unsigned long)pfnOld - sizeof(jmpCodeTemplate);

    m_pfnOld  =  pfnOld;

    memcpy((void*)m_byOld, (void*)m_pfnOld, sizeof(jmpCodeTemplate));
}

/////////////////////////////////////////////////////////////////
bool ArchApiHookImpl::changeCode(char* code)
{
	return modifier->modify((void *)m_pfnOld, code,sizeof(jmpCodeTemplate));
}

/////////////////////////////////////////////////////////////////
void ArchApiHookImpl::startHook()
{
	ArchApiHookImpl::changeCode((char*)m_byNew);
}

/////////////////////////////////////////////////////////////////
void ArchApiHookImpl::stopHook()
{
	ArchApiHookImpl::changeCode((char*)m_byOld);
}

/////////////////////////////////////////////////////////////////
void ArchApiHookImpl::hook(ApiHook::Address pfnOld, ApiHook::Address pfnNew, bool isStdcall)
{
	if (!allocThunk())
	{
		return;
	}

    initHook(pfnOld, pfnNew);
	initThunk(pfnOld, pfnNew, isStdcall);

	startHook();
}

/////////////////////////////////////////////////////////////////
ArchApiHookImpl::~ArchApiHookImpl()
{
	stopHook();
	freeThunk(); // TODO: it must call dtor before destroying the members.

	delete allocator;
    delete modifier;
}

/////////////////////////////////////////////////////////////////
ArchApiHook::ArchApiHook(PageAllocator *pageAllocator, CodeModifier *codeModifier)
	: This(new ArchApiHookImpl(pageAllocator, codeModifier))
{
}

/////////////////////////////////////////////////////////////////
ArchApiHook::~ArchApiHook()
{
	delete This;
}

/////////////////////////////////////////////////////////////////
void ArchApiHook::hook(ApiHook::Address pfnOld, ApiHook::Address pfnNew, bool isStdcall )
{
	This->hook(pfnOld, pfnNew, isStdcall);
}

/////////////////////////////////////////////////////////////////

MOCKCPP_NS_END

