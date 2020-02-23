import sys, os, time, subprocess
import argparse

proc_list = []

def cmd(exe, cpu_range, port_range, gid):
  port_binary_list = ''.join(['1' if p in port_range else '0' for p in range(0,8)])[::-1]
  # port_binary_str = '0' * (8-ports) + '1' * ports
  port_hex_str = hex(int(port_binary_str, 2))
  thread_num = len(cpu_range)
  for i in thread_num:
    exe_str = 'sudo ' + exe
    arg_str =  f' -l {cpu_range[i]} -n 4 --proc-type=auto --file-prefix=g{gid}'
    arg_str += f' -- -p {port_hex_str} --num-procs={thread_num} --proc-id={i}'
    # arg_str += ' &'
    print(exe_str+arg_str)
    while True:
      proc = subprocess.Popen([exe_str+arg_str], shell=True)
      # os.system(exe_str+arg_str)
      c = input()
      if c == '':
        proc_list.append(proc)
        break
      proc.terminate()

  print(f'All processes in Group {gid} are running')


to_kill = False

def kill(exe):
  for p in proc_list:
    p.terminate()
  # exe_name = exe.split('/')[-1]
  # cmd_str = f'sudo killall {exe_name}'
  # print(cmd_str)
  # os.system(cmd_str)

def exit(signum, frame):
  print('Ready to exit.')
  to_kill = True

# python3 run_dpdk.py ./rubik {cpu:'1-4',port='0-1'} {cpu:'5-8',port='2-3'}
if __name__ == '__main__':
  assert sys.argc > 2
  signal.signal(signal.SIGINT, exit)

  exe = sys.argv[1]
  for i in range(2, sys.argc):
    try:
      cp_map = eval(sys.argv[i])
    except:
      print (f'Wrong format of group: {sys.argv[i]}')
      exit()
    cpu_range = range(cp_map[cpu].split('-')[0], cp_map[cpu].split('-')[1]+1)
    port_range = range(cp_map[port].split('-')[0], cp_map[port].split('-')[1]+1)
    cmd (exe, cpu_range, port_range, i)

  print('All processes are running')

  while True:
    if to_kill:
      kill(exe)
      break

  print(f'{exe} has been killed')