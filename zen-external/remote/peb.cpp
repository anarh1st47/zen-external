#include "peb.hpp"
#include <remote/process.hpp>
#include <base/auto_alloc.hpp>
#include <winnls.h>

namespace remote
{
	uintptr_t module_t::get_proc_address(const char* export_name)
	{
		for (const auto& exp : exports)
		{
			if (exp.first == export_name)
				return exp.second;
		}

		return 0;
	}

	peb_t::peb_t(process_t* process):
		process_(process),
		pbi_(),
		peb_(),
		ldr_(),
		process_parameters_()
	{
	}

	bool peb_t::read()
	{
		if (!pbi_.PebBaseAddress)
		{
			if (!process_->query_information(pbi_, ProcessBasicInformation))
				return false;
		}

		if (!process_->read(uintptr_t(pbi_.PebBaseAddress), peb_))
			return false;

		if (!process_->read(uintptr_t(peb_.Ldr), ldr_))
			return false;
		peb_.Ldr = &ldr_;

		
		if (!process_->read(uintptr_t(peb_.ProcessParameters), process_parameters_))
			return false;
		peb_.ProcessParameters = &process_parameters_;

		command_line_ = from_unicode_string(process_parameters_.CommandLine);

		uintptr_t first_link = uintptr_t(ldr_.InLoadOrderModuleList.Flink);
		uintptr_t forward_link = first_link;
		do
		{
			LDR_DATA_TABLE_ENTRY entry;
			process_->read(forward_link, entry);
			forward_link = uintptr_t(entry.InLoadOrderModuleList.Flink);
			
			if (!entry.BaseAddress)
				continue;
			
			module_t mod;
			mod.name = from_unicode_string(entry.BaseDllName);
			mod.base = uintptr_t(entry.BaseAddress);

			// read nt shit
			IMAGE_DOS_HEADER dos_hdr;
			if (!process_->read(mod.base, dos_hdr) || dos_hdr.e_magic != IMAGE_DOS_SIGNATURE)
				continue;
			if (!process_->read(mod.base + dos_hdr.e_lfanew, mod.nt_headers) || mod.nt_headers.Signature != IMAGE_NT_SIGNATURE)
				continue;

			IMAGE_DATA_DIRECTORY export_data_dir = mod.nt_headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
			if (!export_data_dir.VirtualAddress || !export_data_dir.Size)
				continue;
			IMAGE_EXPORT_DIRECTORY export_dir;
			if (!process_->read(mod.base + export_data_dir.VirtualAddress, export_dir) || !export_dir.NumberOfFunctions)
				continue;

			base::auto_alloc_t<uintptr_t> functions(export_dir.NumberOfFunctions);
			base::auto_alloc_t<uintptr_t> names(export_dir.NumberOfNames);
			base::auto_alloc_t<uint16_t> ordinals(export_dir.NumberOfNames);

			process_->read_memory(mod.base + export_dir.AddressOfFunctions, functions, functions.size());
			process_->read_memory(mod.base + export_dir.AddressOfNames, names, names.size());
			process_->read_memory(mod.base + export_dir.AddressOfNameOrdinals, ordinals, ordinals.size());

			for (size_t i = 0u; i < export_dir.NumberOfNames; ++i)
			{
				char buffer[128];
				process_->read_memory(mod.base + names[i], buffer, 128);

				mod.exports.emplace_back(std::make_pair(buffer, mod.base + functions[ordinals[i]]));
			}

			modules_.push_back(mod);
		} while (forward_link && forward_link != first_link);

		return true;
	}

	module_t* peb_t::get_module(const char* module_name)
	{
		for (auto& module : modules_)
		{
			if (module.name == module_name)
				return &module;
		}
		return nullptr;
	}

	bool peb_t::reset()
	{
		::memset(&peb_, 0, sizeof peb_);
		return read();
	}

	std::string peb_t::from_unicode_string(const UNICODE_STRING& value) const
	{
		std::unique_ptr<wchar_t[]> buffer(std::make_unique<wchar_t[]>(value.Length));
		if (!process_->read_memory(uintptr_t(value.Buffer), buffer.get(), value.Length))
			return "";

		int size = WideCharToMultiByte(CP_UTF8, 0, buffer.get(), value.Length, nullptr, 0, nullptr, nullptr);
		std::string ret;
		ret.reserve(size);
		WideCharToMultiByte(CP_UTF8, 0, buffer.get(), value.Length, ret.data(), size, nullptr, nullptr);

		return ret;
	}
} // namespace remote