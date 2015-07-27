////////////////////////////////////////////////////////////////////////////////////////////
// loader: command-line interface dll injector
// Copyright (C) 2009-2011 Wadim E. <wdmegrv@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
////////////////////////////////////////////////////////////////////////////////////////////
#include "injectory/manualmap.hpp"
#include "injectory/common.hpp"
#include "injectory/exception.hpp"
#include "injectory/dllmain_remotecall.hpp"
#include "injectory/generic_injector.hpp"
#include "injectory/injector_helper.hpp"
#include "injectory/process.hpp"

#include <stdio.h>
#include <Windows.h>
#include <TlHelp32.h>

using namespace std;


// Matt Pietrek's function
PIMAGE_SECTION_HEADER GetEnclosingSectionHeader(DWORD_PTR rva, PIMAGE_NT_HEADERS pNTHeader)
{
	PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(pNTHeader);
	WORD nSection = 0;

	for(nSection = 0; nSection < pNTHeader->FileHeader.NumberOfSections; nSection++, section++ )
	{
		// This 3 line idiocy is because Watcom's linker actually sets the
		// Misc.VirtualSize field to 0.  (!!! - Retards....!!!)
		DWORD_PTR size = section->Misc.VirtualSize;
		if(size == 0)
		{
			size = section->SizeOfRawData;
		}

		// Is the RVA within this section?
		if( (rva >= section->VirtualAddress) && (rva < (section->VirtualAddress + size)) )
		{
			return section;
		}
	}

	return 0;
}

// Matt Pietrek's function
LPVOID GetPtrFromRVA(DWORD_PTR rva, PIMAGE_NT_HEADERS pNTHeader, PBYTE imageBase)
{
	PIMAGE_SECTION_HEADER section;
	LONG_PTR delta;

	section = GetEnclosingSectionHeader(rva, pNTHeader);
	if(!section)
	{
		return 0;
	}

	delta = (LONG_PTR)( section->VirtualAddress - section->PointerToRawData );
	return (LPVOID)( imageBase + rva - delta );
}

void FixIAT(
	pid_t dwProcessId,
	HANDLE hProcess,
	PBYTE imageBase,
	PIMAGE_NT_HEADERS pNtHeader,
	PIMAGE_IMPORT_DESCRIPTOR pImgImpDesc
	)
{
	LPSTR lpModuleName = 0;
	HMODULE hRemoteModule = 0;
	WCHAR modulePath[MAX_PATH + 1] = {0};
	WCHAR moduleNtPath[500 + 1] = {0};
	WCHAR targetProcPath[MAX_PATH + 1] = {0};
	WCHAR *pch = 0;

	try
	{
		//printf("Fixing Imports:\n");

		// get target process path
		if(!GetModuleFileNameExW(hProcess, (HMODULE)0, targetProcPath, MAX_PATH))
			BOOST_THROW_EXCEPTION (ex_fix_iat() << e_text("could not get path to target process"));

		pch = wcsrchr(targetProcPath, '\\');
		if(pch)
			targetProcPath[ pch - targetProcPath + 1 ] = (WCHAR)0;

		if(!SetDllDirectoryW(targetProcPath))
			BOOST_THROW_EXCEPTION (ex_fix_iat() << e_text("could not set path to target process") << e_file_path(targetProcPath));

		while((lpModuleName = (LPSTR)GetPtrFromRVA(pImgImpDesc->Name, pNtHeader, imageBase)))
		{
			PIMAGE_THUNK_DATA itd = 0;

			//printf("module: %s\n", lpModuleName);

			// ACHTUNG: LoadLibraryEx kann eine DLL nur anhand des Namen aus einem anderen
			// Verzeichnis laden wie der Zielprozess!
			shared_ptr<void> hLocalModule(LoadLibraryExA(lpModuleName, 0, DONT_RESOLVE_DLL_REFERENCES), FreeLibrary);
			if(!hLocalModule)
				BOOST_THROW_EXCEPTION (ex_fix_iat() << e_text("could not load module locally"));

			// get full path of module
			if(!GetModuleFileNameW((HMODULE)hLocalModule.get(), modulePath, MAX_PATH))
				BOOST_THROW_EXCEPTION (ex_fix_iat() << e_text("could not get path to module") << e_file_path(lpModuleName));

			// get nt path
			if(!GetFileNameNtW(modulePath, moduleNtPath, 500))
				BOOST_THROW_EXCEPTION (ex_fix_iat() << e_text("could not get the NT namespace path"));

			// Module already in process?
			hRemoteModule = (HMODULE)ModuleInjectedW(hProcess, moduleNtPath);
			if(!hRemoteModule)
			{
				Process::open(dwProcessId).inject(modulePath);
				
				hRemoteModule = (HMODULE)ModuleInjectedW(hProcess, moduleNtPath);
			}

			itd = (PIMAGE_THUNK_DATA)GetPtrFromRVA(pImgImpDesc->FirstThunk, pNtHeader, imageBase);

			while(itd->u1.AddressOfData)
			{
				IMAGE_IMPORT_BY_NAME *iibn =
					(PIMAGE_IMPORT_BY_NAME)GetPtrFromRVA(itd->u1.AddressOfData, pNtHeader, imageBase);
				itd->u1.Function = (DWORD_PTR)GetRemoteProcAddress(hProcess, hRemoteModule, (LPCSTR)iibn->Name);

				//printf("Function: %s\n", (LPCSTR)iibn->Name);

				itd++;
			}      

			pImgImpDesc++;
		}
	}
	catch (const boost::exception& e)
	{
		e << e_text("error fixing imports");
		throw;
	}
}

BOOL
MapSections(
	HANDLE hProcess,
	LPVOID lpModuleBase,
	PBYTE dllBin,
	PIMAGE_NT_HEADERS pNTHeader
	)
{
	PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(pNTHeader);
	SIZE_T virtualSize = 0;
	WORD nSection = 0;
	
	for(nSection = 0; nSection < pNTHeader->FileHeader.NumberOfSections; nSection++)
	{
		LPVOID lpBaseAddress = (LPVOID)( (DWORD_PTR)lpModuleBase + section->VirtualAddress );
		LPCVOID lpBuffer = (LPCVOID)( (DWORD_PTR)dllBin + section->PointerToRawData );
		SIZE_T NumBytesWritten = 0;
		PDWORD lpflOldProtect = 0;

		if(!WriteProcessMemory(hProcess, lpBaseAddress, lpBuffer, section->SizeOfRawData, &NumBytesWritten) ||
			NumBytesWritten != section->SizeOfRawData)
		{
			PRINT_ERROR_MSGA("Could not write to memory in remote process.");
			return FALSE;
		}	
		
		// next section header, calculate virtualSize of section header
		virtualSize = section->VirtualAddress;
		//printf("section: %s | %p | %x\n", section->Name, section->VirtualAddress, virtualSize);
		section++;
		if(section->VirtualAddress)
		{
			virtualSize = section->VirtualAddress - virtualSize;
		}
		/*
		if(!VirtualProtectEx(hProcess, (LPVOID)( (DWORD_PTR)lpModuleBase + section->VirtualAddress ), virtualSize,
			section->Characteristics & 0x00FFFFFF, lpflOldProtect))
		{
			PRINT_ERROR_MSGA("VirtualProtectEx failed.");
			return FALSE;
		}
		*/
	}

	return TRUE;
}

BOOL
FixRelocations(
	PBYTE dllBin,
	LPVOID lpModuleBase,
	PIMAGE_NT_HEADERS pNtHeader,
	PIMAGE_BASE_RELOCATION pImgBaseReloc
	)
{
	LONG_PTR delta = (DWORD_PTR)lpModuleBase - pNtHeader->OptionalHeader.ImageBase;
	SIZE_T relocationSize = pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
	WORD *pRelocData = 0;

	//printf("FixRelocs:\n");

	// image has no relocations
	if(!pImgBaseReloc->SizeOfBlock)
	{
		//printf("Image has no relocations\n");
		return TRUE;
	}
	
	do
	{
		PBYTE pRelocBase = (PBYTE)GetPtrFromRVA(pImgBaseReloc->VirtualAddress, pNtHeader, dllBin);
		SIZE_T numRelocations = (pImgBaseReloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
		SIZE_T i = 0;

		//printf("numRelocations: %d\n", numRelocations);

		pRelocData = (WORD*)( (DWORD_PTR)pImgBaseReloc + sizeof(IMAGE_BASE_RELOCATION) );

		// loop over all relocation entries
		for(i = 0; i < numRelocations; i++, pRelocData++)
		{
			// Get reloc data
			BYTE RelocType = *pRelocData >> 12;
			WORD Offset = *pRelocData & 0xFFF;

			switch(RelocType)
			{
			case IMAGE_REL_BASED_ABSOLUTE:
				break;

			case IMAGE_REL_BASED_HIGHLOW:
				*(DWORD32*)(pRelocBase + Offset) += (DWORD32)delta;
				break;

			case IMAGE_REL_BASED_DIR64:
				*(DWORD64*)(pRelocBase + Offset) += delta;

				break;

			default:
				PRINT_ERROR_MSGA("Unsuppported relocation type.");
				return FALSE;
			}
		}

		pImgBaseReloc = (PIMAGE_BASE_RELOCATION)pRelocData;

	} while( *(DWORD*)pRelocData );

	return TRUE;
}

BOOL
CallTlsInitializers(
	PBYTE imageBase,
	PIMAGE_NT_HEADERS pNtHeader,
	HANDLE hProcess,
	HMODULE hModule,
	DWORD fdwReason,
	PIMAGE_TLS_DIRECTORY pImgTlsDir
	)
{
	DWORD_PTR pCallbacks = (DWORD_PTR)pImgTlsDir->AddressOfCallBacks;

	if(pCallbacks)
	{
		while(TRUE)
		{
			SIZE_T NumBytesRead = 0;
			LPVOID callback = 0;

			if(!ReadProcessMemory(hProcess, (PVOID)pCallbacks, &callback, sizeof(LPVOID), &NumBytesRead) ||
				NumBytesRead != sizeof(LPVOID))
			{
				PRINT_ERROR_MSGA("Could not read memory in remote process.");
				return FALSE;
			}

			if(!callback) break;

			RemoteDllMainCall(hProcess, callback, hModule, fdwReason, 0);
			//printf("callback: %p\n", callback);
			pCallbacks += sizeof(DWORD_PTR);
		}
	}
	return TRUE;
}

void MapRemoteModule(const pid_t& pid, const path& lib)
{
	DWORD fileSize = 0;
	BYTE *dllBin = 0;
	PIMAGE_NT_HEADERS nt_header = 0;
	PIMAGE_DOS_HEADER dos_header = 0;
	LPVOID lpModuleBase = 0;

	PIMAGE_IMPORT_DESCRIPTOR pImgImpDesc = 0;
	PIMAGE_BASE_RELOCATION pImgBaseReloc = 0;
	PIMAGE_TLS_DIRECTORY pImgTlsDir = 0;

	try
	{
		// Get a handle for the target process.
		shared_ptr<void> hProcess(OpenProcess(
			PROCESS_QUERY_INFORMATION	|	// Required by Alpha
			PROCESS_CREATE_THREAD		|	// For CreateRemoteThread
			PROCESS_VM_OPERATION		|	// For VirtualAllocEx/VirtualFreeEx
			PROCESS_VM_WRITE			|	// For WriteProcessMemory
			PROCESS_VM_READ,
			FALSE, 
			pid),
			CloseHandle);
		if(!hProcess.get())
			BOOST_THROW_EXCEPTION (ex_map_remote() << e_text("could not get handle to process") << e_pid(pid));

		shared_ptr<void> hFile(CreateFileW(
			lib.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			NULL),
			CloseHandle);
		if(hFile.get() == INVALID_HANDLE_VALUE)
			BOOST_THROW_EXCEPTION (ex_map_remote() << e_text("CreateFileW failed"));

		if(GetFileAttributesW(lib.c_str()) & FILE_ATTRIBUTE_COMPRESSED)
			fileSize = GetCompressedFileSizeW(lib.c_str(), NULL);
		else
			fileSize = GetFileSize(hFile.get(), NULL);

		if(fileSize == INVALID_FILE_SIZE)
			BOOST_THROW_EXCEPTION (ex_map_remote() << e_text("could not get size of file"));

		shared_ptr<BYTE> dllBin((BYTE*)malloc(fileSize), free);

		{
			DWORD NumBytesRead = 0;
			if(!ReadFile(hFile.get(), dllBin.get(), fileSize, &NumBytesRead, FALSE))
				BOOST_THROW_EXCEPTION (ex_map_remote() << e_text("ReadFile failed"));
		}
	
		dos_header = (PIMAGE_DOS_HEADER)dllBin.get();
		
		// Make sure we got a valid DOS header
		if(dos_header->e_magic != IMAGE_DOS_SIGNATURE)
			BOOST_THROW_EXCEPTION (ex_map_remote() << e_text("invalid DOS header"));
		
		// Get the real PE header from the DOS stub header
		nt_header = (PIMAGE_NT_HEADERS)( (DWORD_PTR)dllBin.get() +
			dos_header->e_lfanew);

		// Verify the PE header
		if(nt_header->Signature != IMAGE_NT_SIGNATURE)
			BOOST_THROW_EXCEPTION (ex_map_remote() << e_text("invalid PE header"));

		// Allocate space for the module in the remote process
		lpModuleBase = VirtualAllocEx(
			hProcess.get(),
			NULL, 
			nt_header->OptionalHeader.SizeOfImage, 
			MEM_COMMIT | MEM_RESERVE, 
			PAGE_EXECUTE_READWRITE);
		if(!lpModuleBase)
			BOOST_THROW_EXCEPTION (ex_map_remote() << e_text("could not allocate memory in remote process"));
		
		// fix imports
		pImgImpDesc = (PIMAGE_IMPORT_DESCRIPTOR)GetPtrFromRVA(
			nt_header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress,
			nt_header,
			(PBYTE)dllBin.get());
		if (nt_header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size)
			FixIAT(pid, hProcess.get(), (PBYTE)dllBin.get(), nt_header, pImgImpDesc);
		
		// fix relocs
		pImgBaseReloc = (PIMAGE_BASE_RELOCATION)GetPtrFromRVA(
			(DWORD)(nt_header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress),
			nt_header,
			(PBYTE)dllBin.get());
		if(nt_header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size)
		{
			if(!FixRelocations(dllBin.get(), lpModuleBase, nt_header, pImgBaseReloc))
				BOOST_THROW_EXCEPTION (ex_map_remote() << e_text("error fixing relocations"));
		}

		// Write the PE header into the remote process's memory space
		{
			SIZE_T NumBytesWritten = 0;
			SIZE_T nSize = nt_header->FileHeader.SizeOfOptionalHeader +
				sizeof(nt_header->FileHeader) +
				sizeof(nt_header->Signature);
			
			if(!WriteProcessMemory(hProcess.get(), lpModuleBase, dllBin.get(), nSize, &NumBytesWritten) ||
					NumBytesWritten != nSize)
				BOOST_THROW_EXCEPTION (ex_map_remote() << e_text("could not write to memory in remote process"));
		}

		// Map the sections into the remote process(they need to be aligned
		// along their virtual addresses)
		if(!MapSections(hProcess.get(), lpModuleBase, dllBin.get(), nt_header))
			BOOST_THROW_EXCEPTION (ex_map_remote() << e_text("error mapping sections"));

		// call all tls callbacks
		//
		pImgTlsDir = (PIMAGE_TLS_DIRECTORY)GetPtrFromRVA(
			nt_header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress,
			nt_header,
			(PBYTE)dllBin.get());
		if(nt_header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size)
		{
			if(!CallTlsInitializers(dllBin.get(), nt_header, hProcess.get(), (HMODULE)lpModuleBase, DLL_PROCESS_ATTACH, pImgTlsDir))
				BOOST_THROW_EXCEPTION(ex_map_remote() << e_text("@Call TLS initializers."));
		}

		// call entry point
		if(!RemoteDllMainCall(
				hProcess.get(),
				(LPVOID)( (DWORD_PTR)lpModuleBase + nt_header->OptionalHeader.AddressOfEntryPoint),
				(HMODULE)lpModuleBase, 1, 0))
			BOOST_THROW_EXCEPTION (ex_map_remote() << e_text("@Call DllMain."));

		wprintf(
			L"Successfully injected (%s | PID: %d):\n\n"
			L"  AllocationBase: 0x%p\n"
			L"  EntryPoint:     0x%p\n"
			L"  SizeOfImage:      %.1f kB\n"
			L"  CheckSum:       0x%08x\n",
			lib.c_str(),
			pid,
			lpModuleBase,
			(LPVOID)((DWORD_PTR)lpModuleBase + nt_header->OptionalHeader.AddressOfEntryPoint),
			nt_header->OptionalHeader.SizeOfImage/1024.0,
			nt_header->OptionalHeader.CheckSum);
	}
	catch (const boost::exception& e)
	{
		e << e_text("failed to map the PE file into the remote address space of a process") << e_file_path(lib) << e_pid(pid);
		throw;
	}
}