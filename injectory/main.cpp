#include "injectory/common.hpp"
#include "injectory/exception.hpp"
#include "injectory/findproc.hpp"
#include "injectory/library.hpp"
#include "injectory/process.hpp"
#include "injectory/module.hpp"
#include "injectory/job.hpp"

#include <boost/program_options.hpp>
namespace po = boost::program_options;

#define VERSION "5.0-SNAPSHOT"

Process proc;

int main(int argc, char *argv[])
{
	po::variables_map vars;
	try
	{
		po::options_description desc(
			"usage: injectory [OPTION]...\n"
			"inject DLL:s into processes\n"
			"<exe> and <dll> can be relative paths\n"
			"\n"
			"Examples:\n"
			"  injectory -l a.exe -i b.dll --args \"1 2 3\" --wii\n"
			"  injectory -p 12345 -i b.dll --mm --wait-for-exit\n"
			"\n"
			"Options");

		desc.add_options()
			("pid,p",		po::value<int>()->value_name("PID"),		"injection via process id")
			//("procname",	po::value<string>()->value_name("NAME"),	"injection via process name")
			//("wndtitle",	po::value<string>()->value_name("TITLE"),	"injection via window title")
			//("wndclass",	po::value<string>()->value_name("CLASS"),	"injection via window class")
			("launch,l",	po::value<path>()->value_name("EXE"),		"launches the target in a new process")
			("args,a",		po::wvalue<wstring>()->value_name("STRING")->default_value(L"", ""),
																		"arguments for --launch:ed process\n")
			
			("inject,i",	po::value<vector<path>>()->value_name("DLL...")->multitoken()->default_value(vector<path>(),""),
																		"inject libraries before main")
			("injectw,I",	po::value<vector<path>>()->value_name("DLL...")->multitoken()->default_value(vector<path>(), ""),
																		"inject libraries when input idle")
			("map,m",		po::value<vector<path>>()->value_name("DLL...")->multitoken()->default_value(vector<path>(),""),
																		"map file into target before main")
			("mapw,M",		po::value<vector<path>>()->value_name("DLL...")->multitoken()->default_value(vector<path>(), ""),
																		"map file into target when input idle")
			("eject,e",		po::value<vector<path>>()->value_name("DLL...")->multitoken()->default_value(vector<path>(), ""),
																		"eject libraries before main")
			("ejectw,E",	po::value<vector<path>>()->value_name("DLL...")->multitoken()->default_value(vector<path>(), ""),
																		"eject libraries when input idle\n")

			("print-own-pid",											"print the pid of this process")
			("print-pid",												"print the pid of the target process")
			("rethrow",													"rethrow exceptions")
			("vs-debug-workaround",									  	"workaround threads left suspended when debugging with"
																		" visual studio by resuming all threads for 2 seconds")

			("dbgpriv",												  	"set SeDebugPrivilege")
			("wait-for-exit",											"wait for the target to exit before exiting")
			("kill-on-exit",											"kill the target when exiting\n")

			("verbose,v",												"")
			("version",													"display version information and exit")
			("help",													"display help message and exit")
			//("Address of library (ejection)")
			//("a process (without calling LoadLibrary)")
			//("listmodules",									"dump modules associated with the specified process id")
		;

		po::store(po::parse_command_line(argc, argv, desc), vars);
		po::notify(vars);

		if (vars.count("help"))
		{
			cout << desc << endl;
			return 0;
		}
		
		if (vars.count("version"))
		{
			cout << "injectory " << VERSION << endl
				 << "project home: https://github.com/blole/injectory" << endl;
			return 0;
		}

		if (vars.count("print-own-pid"))
			cout << Process::current.id() << endl;

		if (vars.count("dbgpriv"))
			Process::current.enablePrivilege(L"SeDebugPrivilege");

		bool verbose = vars.count("verbose") > 0;

		if (vars.count("pid"))
		{
			int pid = vars["pid"].as<int>();
			proc = Process::open(pid);
			proc.suspend();
		}
		else if (vars.count("launch"))
		{
			using boost::none;
			path    app  = vars["launch"].as<path>();
			wstring args = vars["args"].as<wstring>();
			
			proc = Process::launch(app, args, none, none, false, CREATE_SUSPENDED).process;
		}

		/*
		else if (vars.count("procname"))
		InjectEjectToProcessNameA(var_string("procname"), lib, nullptr, !eject, mm);
		else if (vars.count("wndtitle"))
		InjectEjectToWindowTitleA(var_string("wndtitle"), lib, nullptr, !eject, mm);
		else if (vars.count("wndclass"))
		InjectEjectToWindowClassA(var_string("wndclass"), lib, nullptr, !eject, mm);
		*/

		if (proc)
		{
			if (proc.is64bit() != is64bit)
				BOOST_THROW_EXCEPTION(ex_target_bit_mismatch() << e_pid(proc.id()));

			Job job;
			if (vars.count("kill-on-exit"))
			{
				job = Job::create();
				job.assignProcess(proc);
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = { 0 };
				jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				job.setInfo(JobObjectExtendedLimitInformation, jeli);
			}

			for (const Library& lib : vars["inject"].as<vector<path>>())
				proc.inject(lib, verbose);

			for (const Library& lib : vars["map"].as<vector<path>>())
				proc.mapRemoteModule(lib, verbose);

			for (const Library& lib : vars["eject"].as<vector<path>>())
				proc.getInjected(lib).eject();

			proc.resume();
			if (!vars["injectw"].empty() || !vars["mapw"].empty() || !vars["ejectw"].empty())
				proc.waitForInputIdle(5000);

			for (const Library& lib : vars["injectw"].as<vector<path>>())
				proc.inject(lib, verbose);

			for (const Library& lib : vars["mapw"].as<vector<path>>())
				proc.mapRemoteModule(lib, verbose);

			for (const Library& lib : vars["ejectw"].as<vector<path>>())
				proc.getInjected(lib).eject();

			if (vars.count("vs-debug-workaround"))
			{
				//resume threads that may have been left suspended when debugging with visual studio
				for (int i = 0; i < 20; i++)
				{
					proc.wait(100);
					proc.resumeAllThreads();
				}
				cout << "done" << endl;
			}

			if (vars.count("print-pid"))
				cout << proc.id() << endl;

			if (vars.count("wait-for-exit"))
				proc.wait();

			if (vars.count("kill-on-exit"))
				proc.kill();
		}
	}
	catch (const boost::exception& e)
	{
		cerr << boost::diagnostic_information(e);
		Sleep(1000);
		if (vars.count("rethrow"))
			throw;
	}
	catch (const exception& e)
	{
		cerr << "non-boost exception caught: " << e.what() << endl;
		if (vars.count("rethrow"))
			throw;
	}
	catch (...)
	{
		cerr << "exception of unknown type" << endl;
		if (vars.count("rethrow"))
			throw;
	}

	return 0;
}
