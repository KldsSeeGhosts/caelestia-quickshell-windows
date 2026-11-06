#include "processcore.hpp"

#include <qcontainerfwd.h>
#include <qhash.h>
#include <qprocess.h>
#include <qvariant.h>
#include <memory>
#include <vector>

#include "../core/common.hpp"

#ifdef _WIN32
#include <qt_windows.h>
#endif

namespace qs::io::process {

void setupProcessEnvironment(
    QProcess* process,
    bool clear,
    const QHash<QString, QVariant>& envChanges
) {
	const auto& sysenv = qs::Common::INITIAL_ENVIRONMENT;
	auto env = clear ? QProcessEnvironment() : sysenv;

	for (auto& name: envChanges.keys()) {
		auto value = envChanges.value(name);
		if (!value.isValid()) continue;

		if (clear) {
			if (value.isNull()) {
				if (sysenv.contains(name)) env.insert(name, sysenv.value(name));
			} else env.insert(name, value.toString());
		} else {
			if (value.isNull()) env.remove(name);
			else env.insert(name, value.toString());
		}
	}

	process->setProcessEnvironment(env);
}

void disownHandles(QProcess* process) {
#ifdef _WIN32
	process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
		args->inheritHandles = false;
		args->startupInfo->dwFlags &= ~static_cast<DWORD>(STARTF_USESTDHANDLES);
		args->startupInfo->hStdInput = nullptr;
		args->startupInfo->hStdOutput = nullptr;
		args->startupInfo->hStdError = nullptr;
	});
#else
	Q_UNUSED(process);
#endif
}

void ownHelper(QProcess* process) {
#ifdef _WIN32
	// Only attached Process instances belong to this job. Detached applications
	// never enter it. Assignment at creation avoids a crash-before-assignment race.
	struct Job {
		HANDLE handle = CreateJobObjectW(nullptr, nullptr);
		Job() {
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
			limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (handle && !SetInformationJobObject(handle, JobObjectExtendedLimitInformation,
			                                    &limits, sizeof(limits))) {
				CloseHandle(handle);
				handle = nullptr;
			}
		}
		~Job() { if (handle) CloseHandle(handle); }
	};
	struct Attributes {
		STARTUPINFOEXW startup {};
		std::vector<unsigned char> storage;
		bool initialized = false;
		~Attributes() {
			if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
		}
	};
	static Job job;
	auto attributes = std::make_shared<Attributes>();
	process->setCreateProcessArgumentsModifier([attributes](QProcess::CreateProcessArguments* args) {
		SIZE_T size = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
		attributes->storage.resize(size);
		attributes->startup.StartupInfo = *args->startupInfo;
		attributes->startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
		attributes->startup.lpAttributeList =
		    reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes->storage.data());
		attributes->initialized = job.handle && size &&
		    InitializeProcThreadAttributeList(attributes->startup.lpAttributeList, 1, 0, &size);
		if (!attributes->initialized ||
		    !UpdateProcThreadAttribute(attributes->startup.lpAttributeList, 0,
		                               PROC_THREAD_ATTRIBUTE_JOB_LIST, &job.handle,
		                               sizeof(job.handle), nullptr, nullptr)) {
			// Fail closed: an unowned helper must not survive the shell.
			args->applicationName = L"";
			return;
		}
		args->flags |= EXTENDED_STARTUPINFO_PRESENT;
		args->startupInfo = &attributes->startup.StartupInfo;
	});
#else
	Q_UNUSED(process);
#endif
}

} // namespace qs::io::process
