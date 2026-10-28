CSOPESY MO1 - OS Emulator: Process Scheduler and CLI

Group members:
- Member 1
- Member 2
- Member 3
- Member 4

Entry file: main.cpp (the main function is at the bottom of this file)

How to build in VS Code (needs WinLibs g++ in C:\winlibs\bin):
  Open this folder in VS Code and press F5. It builds csopesy.exe and opens it
  in a console window. If your WinLibs is somewhere else, change the paths in
  .vscode/tasks.json and .vscode/launch.json

  Note: the old MinGW g++ (version 6.3) does not support std::thread, so use
  WinLibs or another recent compiler

How to build from a terminal:
  g++ -std=c++17 -O2 -static main.cpp -o csopesy

How to run:
  1. Keep config.txt in the same folder you run csopesy.exe from
  2. Run csopesy.exe
  3. Type "initialize" first, then use the other commands

Main menu commands:
  initialize          reads config.txt and starts the CPU scheduler
  screen -s <name>    creates a new process and opens its screen
  screen -r <name>    reopens the screen of a process that hasn't finished
  screen -ls          shows CPU utilization and running/finished processes
  scheduler-start     starts making dummy processes every batch-process-freq ticks
  scheduler-stop      stops making dummy processes
  report-util         saves the screen -ls output to csopesy-log.txt
  exit                closes the emulator

Process screen commands:
  process-smi         shows the process info and its logs
  exit                goes back to the main menu

Notes:
  - One CPU tick is 100 ms of real time (TICK_MS at the top of main.cpp)
  - FOR loops are unrolled into a flat instruction list when a process is created
