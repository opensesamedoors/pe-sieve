#include "workingset_scanner.h"
#include "module_data.h"
#include "artefact_scanner.h"
#include "scanner.h"

#include "../utils/path_converter.h"
#include "../utils/workingset_enum.h"
#include "../utils/artefacts_util.h"

using namespace pesieve;
using namespace pesieve::util;


namespace pesieve {

	bool is_by_stats(const t_shellc_mode& shellc_mode)
	{
		switch (shellc_mode) {
		case  SHELLC_STATS:
		case  SHELLC_PATTERNS_OR_STATS:
		case SHELLC_PATTERNS_AND_STATS:
			return true;
		}
		return false;
	}

	bool is_by_patterns(const t_shellc_mode& shellc_mode)
	{
		switch (shellc_mode) {
		case  SHELLC_PATTERNS:
		case  SHELLC_PATTERNS_OR_STATS:
		case SHELLC_PATTERNS_AND_STATS:
			return true;
		}
		return false;
	}
};

bool pesieve::WorkingSetScanner::checkAreaContent(IN MemPageData& memPage, OUT WorkingSetScanReport* my_report)
{
	if (!memPage.load()) {
		return false;
	}

	const bool noPadding = true;

	bool isByStats = is_by_stats(this->args.shellcode);
	bool isByPatterns = is_by_patterns(this->args.shellcode);

	bool code = false;
	bool codeP = false;
	bool codeS = false;
	bool obfuscated = false;
	if (isByPatterns) {
		if (is_code(memPage.getLoadedData(noPadding), memPage.getLoadedSize(noPadding))) {
			codeP = true;
			code = true;
			if (this->args.shellcode == SHELLC_PATTERNS_OR_STATS) {
				isByStats = false; // condition satisfied, no more checks required
			}
		}
		else {
			if (this->args.shellcode == SHELLC_PATTERNS_AND_STATS) {
				isByStats = false; // condition NOT satisfied, no more checks required
			}
		}
	}
#ifdef CALC_PAGE_STATS
	if (isByStats || this->args.obfuscated) {
		
		// fill default settings
		MultiStatsSettings settings;
		stats::fillCodeStrings(settings.watchedStrings);

		AreaStatsCalculator calc(memPage.loadedData);
		if (calc.fill(my_report->stats, &settings)) {

			pesieve::RuleMatchersSet codeMatcher(RuleMatcher::RULE_CODE);
			if (codeMatcher.findMatches(my_report->stats, my_report->area_info)) {
				codeS = true;
				code = true;
			}

			if (!codeS && (this->args.obfuscated != OBFUSC_NONE)) {
				int rules = 0;
				if (this->args.obfuscated == OBFUSC_ANY) rules = RuleMatcher::RULE_OBFUSCATED | RuleMatcher::RULE_ENCRYPTED;
				if (this->args.obfuscated == OBFUSC_STRONG_ENC) rules = RuleMatcher::RULE_ENCRYPTED;
				if (this->args.obfuscated == OBFUSC_WEAK_ENC) rules = RuleMatcher::RULE_OBFUSCATED;
				pesieve::RuleMatchersSet obfMatcher(rules);
				if (obfMatcher.findMatches(my_report->stats, my_report->area_info)) {
					obfuscated = true;
					// filter out cache:
					if (memPage.mapping_type == MEM_MAPPED // mapped memory
						&& !util::is_executable(memPage.mapping_type, memPage.protection) // non executable page
						&& memPage.loadMappedName()) //named
					{
						obfuscated = false;
					}
				}
			}
		}
	}
#endif

	if (this->args.shellcode == SHELLC_PATTERNS_AND_STATS) {
		code = (codeP && codeS);
	}
	else if (this->args.shellcode == SHELLC_PATTERNS_OR_STATS) {
		code = (codeP || codeS);
	}
	my_report->has_shellcode = code;

	if ( (this->args.obfuscated != OBFUSC_NONE && obfuscated) || ((this->args.shellcode != SHELLC_NONE) && code) ){
		my_report->status = SCAN_SUSPICIOUS;
		my_report->data_cache = memPage.loadedData;
	}
	return true;
}

bool pesieve::WorkingSetScanner::isExecutable(MemPageData &memPage)
{
	if (pesieve::util::is_executable(memPage.mapping_type, memPage.protection)) {
		return true;
	}
	return isPotentiallyExecutable(memPage, this->args.data);
}

bool pesieve::WorkingSetScanner::isPotentiallyExecutable(MemPageData &memPage, const t_data_scan_mode &mode)
{
	if (mode == pesieve::PE_DATA_NO_SCAN) {
		return false;
	}

	// check preconditions:
	const bool is_managed = this->processReport.isManagedProcess();
	if (mode == pesieve::PE_DATA_SCAN_NO_DEP 
		&& this->pDetails.isDEP && !is_managed)
	{
		return false;
	}
	if (mode == pesieve::PE_DATA_SCAN_DOTNET
		&& !is_managed)
	{
		return false;
	}
	// preconditions are fulfilled, now check the access:
	const bool is_page_readable = pesieve::util::is_readable(memPage.mapping_type, memPage.protection);
	if (mode != pesieve::PE_DATA_SCAN_INACCESSIBLE_ONLY) {
		if (is_page_readable) {
			return true;
		}
	}
	if ((mode >= pesieve::PE_DATA_SCAN_INACCESSIBLE) || (mode == pesieve::PE_DATA_SCAN_INACCESSIBLE_ONLY)) {
		if (this->pDetails.isReflection && (memPage.protection & PAGE_NOACCESS)) {
			return true;
		}
	}
	return false;
}

WorkingSetScanReport* pesieve::WorkingSetScanner::scanExecutableArea(MemPageData &_memPage)
{
	if (!_memPage.load()) {
		return nullptr;
	}
	// check for PE artifacts (regardless if it has shellcode patterns):
	if (!isScannedAsModule(_memPage)) {
		ArtefactScanner artefactScanner(this->processHandle, this->pDetails, _memPage, this->processReport);
		WorkingSetScanReport *my_report1 = artefactScanner.scanRemote();
		if (my_report1) {
			//pe artefacts found
			return my_report1;
		}
	}
	if ((this->args.shellcode == SHELLC_NONE) && (this->args.obfuscated == OBFUSC_NONE)) {
		// not a PE file, and we are not interested in shellcode or obfuscated contents, so just finish it here
		return nullptr;
	}

	//report about shellcode:
	ULONGLONG region_start = _memPage.region_start;
	const size_t region_size = size_t(_memPage.region_end - region_start);
	WorkingSetScanReport* my_report = new WorkingSetScanReport((HMODULE)region_start, region_size, SCAN_NOT_SUSPICIOUS);
	if (!my_report) {
		return nullptr;
	}
	
	if (!checkAreaContent(_memPage, my_report)) { // check for shellcode patterns & stats
		my_report->status = SCAN_ERROR;
	}
	if (my_report->status == SCAN_NOT_SUSPICIOUS) {
		// do not keep reports for not suspicious areas
		delete my_report;
		return nullptr;
	}
	my_report->has_pe = isScannedAsModule(_memPage) && this->processReport.hasModule(_memPage.region_start);
	return my_report;
}

bool pesieve::WorkingSetScanner::isScannedAsModule(MemPageData &memPage)
{
	if (memPage.mapping_type != MEM_IMAGE) {
		return false;
	}
	if (this->processReport.hasModule((ULONGLONG)memPage.alloc_base)) {
		return true; // it was already scanned as a PE
	}
	return false;
}

bool pesieve::WorkingSetScanner::scanImg(MemPageData& memPage)
{
	if (!memPage.loadMappedName()) {
		//cannot retrieve the mapped name
		return false;
	}

	const HMODULE module_start = (HMODULE)memPage.alloc_base;

	if (!args.quiet) {
		std::cout << "[!] Scanning detached: " << std::hex << module_start << " : " << memPage.mapped_name << std::endl;
	}
	RemoteModuleData remoteModData(this->processHandle, this->pDetails.isReflection, module_start);
	if (!remoteModData.isInitialized()) {
		if (!args.quiet) {
			std::cout << "[-] Could not read the remote PE at: " << std::hex << module_start << std::endl;
		}
		return false;
	}

	//load module from file:
	ModuleData modData(processHandle, module_start, memPage.mapped_name, args.use_cache);
	if (!modData.loadOriginal()) {
		if (!args.quiet) {
			std::cerr << "[-] [" << std::hex << modData.moduleHandle << "] Could not read the module file" << std::endl;
		}
		processReport.appendReport(new UnreachableModuleReport(module_start, 0, memPage.mapped_name));
		return false;
	}
	t_scan_status scan_status = ProcessScanner::scanForHollows(processHandle, modData, remoteModData, processReport);
#ifdef _DEBUG
	std::cout << "[*] Scanned for hollows. Status: " << scan_status << std::endl;
#endif
	if (scan_status == SCAN_ERROR) {
		// failed scanning it as a loaded PE module
		return false;
	}
	if (scan_status == SCAN_SUSPICIOUS) {
		// detected as hollowed, no need for further scans
		return true;
	}
	if (!args.no_hooks) {
		const bool scan_data = (this->args.data >= pesieve::PE_DATA_SCAN_ALWAYS && this->args.data != PE_DATA_SCAN_INACCESSIBLE_ONLY)
			|| (!this->pDetails.isDEP && (this->args.data == pesieve::PE_DATA_SCAN_NO_DEP));
		const bool scan_inaccessible = (this->pDetails.isReflection && (this->args.data >= pesieve::PE_DATA_SCAN_INACCESSIBLE));
		scan_status = ProcessScanner::scanForHooks(processHandle, modData, remoteModData, processReport, scan_data, scan_inaccessible);
#ifdef _DEBUG
		std::cout << "[*] Scanned for hooks. Status: " << scan_status << std::endl;
#endif
	}
	return true;
}

WorkingSetScanReport* pesieve::WorkingSetScanner::scanRemote()
{
	MemPageData memPage(this->processHandle, this->pDetails.isReflection, this->memRegion.base, 0);
	memPage.is_listed_module = this->processReport.hasModule(this->memRegion.base);

	if (!memPage.isInfoFilled() && !memPage.fillInfo()) {
#ifdef _DEBUG
		std::cout << "[!] Could not fill: " << std::hex << memPage.start_va << " to: " << memPage.region_end << "\n";
#endif
		return nullptr;
	}
	// sanity checks to make sure that we are scanning the same page that was previously collected:
	if (memPage.alloc_base != this->memRegion.alloc_base) {
#ifdef _DEBUG
		std::cerr << "WARNING: Alloc Base mismatch: " << std::hex << memPage.alloc_base << " vs " << this->memRegion.alloc_base << std::endl;
#endif
		return nullptr;
	}
	if ((memPage.region_end - memPage.region_start) != this->memRegion.size) {
#ifdef _DEBUG
		std::cerr << "WARNING: Size mismatch: " << std::hex << (memPage.region_end - memPage.region_start) << " vs " << this->memRegion.size << std::endl;
#endif
		return nullptr;
	}

	// is the page executable?
	const bool is_any_exec = isExecutable(memPage);
	if (!is_any_exec) {
		// probably not interesting
		return nullptr;
	}

	if (memPage.mapping_type == MEM_MAPPED && memPage.isRealMapping()) {
		//probably legit
		return nullptr;
	}

	if (memPage.mapping_type == MEM_IMAGE) {
		memPage.loadModuleName();
		memPage.loadMappedName();
		if (!isScannedAsModule(memPage)) {
			scanImg(memPage);
		}
		const size_t region_size = (memPage.region_end) ? (memPage.region_end - memPage.region_start) : 0;
		if (this->processReport.hasModuleContaining(memPage.region_start, region_size)) {
			// the area was already scanned
			return nullptr;
		}
	}
#ifdef _DEBUG
	std::cout << std::hex << memPage.start_va << ": Scanning executable area" << std::endl;
#endif
	WorkingSetScanReport* my_report = this->scanExecutableArea(memPage);
	if (!my_report) {
		return nullptr;
	}
	my_report->is_executable = true;
	my_report->protection = memPage.protection;
	my_report->mapping_type = memPage.mapping_type;
	my_report->mapped_name = memPage.mapped_name;
	return my_report;
}
