#!/usr/bin/env python3
import fcntl
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WORKSPACE = Path('/tmp/aura_runtime_workspace')
CURRENT_RUN_PATH = WORKSPACE / 'current_run.json'
HOST = '127.0.0.1'
PORT = 8000
MAX_SOURCE_SIZE = 64_000
MAX_REQUEST_SIZE = 2_000
STEP_VISIBILITY_SECONDS = 1.15
PIPELINE = [
    ('Generator', 'orchestrator_to_generator.fifo', 'generator_to_executor.fifo'),
    ('Executor', 'generator_to_executor.fifo', 'executor_to_reviewer.fifo'),
    ('Reviewer', 'executor_to_reviewer.fifo', 'reviewer_to_tester.fifo'),
    ('Tester', 'reviewer_to_tester.fifo', 'tester_to_monitor.fifo'),
    ('Monitor', 'tester_to_monitor.fifo', None),
]
AGENT_ROLES = {
    'Orchestrator': 'Controller',
    'Generator': 'Worker agent',
    'Executor': 'Worker agent',
    'Reviewer': 'Worker agent',
    'Tester': 'Worker agent',
    'Monitor': 'Controller',
}
PROGRAMS = [
    {'id': 'factorial', 'name': 'Factorial', 'description': 'Calculate factorial of N', 'example': 'factorial of 5'},
    {'id': 'fibonacci', 'name': 'Fibonacci', 'description': 'Print the first N Fibonacci numbers', 'example': 'fibonacci 10'},
    {'id': 'prime', 'name': 'Prime number', 'description': 'Check whether an integer is prime', 'example': 'check if 17 is prime'},
    {'id': 'palindrome', 'name': 'Number palindrome', 'description': 'Check whether an integer reads the same backwards', 'example': 'is 121 a palindrome'},
    {'id': 'sum', 'name': 'Sum of N numbers', 'description': 'Calculate the sum from 1 through N', 'example': 'sum from 1 to 10'},
    {'id': 'even-odd', 'name': 'Even or odd', 'description': 'Determine whether an integer is even or odd', 'example': 'is 42 even or odd'},
    {'id': 'gcd', 'name': 'Greatest common divisor', 'description': 'Find the GCD of two integers', 'example': 'gcd of 24 and 18'},
    {'id': 'lcm', 'name': 'Least common multiple', 'description': 'Find the LCM of two integers', 'example': 'lcm of 12 and 18'},
    {'id': 'reverse', 'name': 'Reverse a number', 'description': 'Reverse the digits of an integer', 'example': 'reverse the number 12345'},
    {'id': 'armstrong', 'name': 'Armstrong number', 'description': 'Check whether an integer is an Armstrong number', 'example': 'is 153 an armstrong number'},
    {'id': 'table', 'name': 'Multiplication table', 'description': 'Print a multiplication table from 1 to 10', 'example': 'multiplication table of 7'},
    {'id': 'maximum', 'name': 'Maximum in an array', 'description': 'Find the maximum value in a sample integer array', 'example': 'find the maximum in an array'},
    {'id': 'sort', 'name': 'Sort an array', 'description': 'Sort a sample integer array in ascending order', 'example': 'sort an array'},
    {'id': 'temperature', 'name': 'Celsius to Fahrenheit', 'description': 'Convert a Celsius temperature to Fahrenheit', 'example': 'convert 25 celsius to fahrenheit'},
    {'id': 'calculator', 'name': 'Basic calculator', 'description': 'Evaluate a basic arithmetic expression', 'example': 'calculate 8 + 2'},
]
ALIASES = {
    'factorial': ('factorial',),
    'fibonacci': ('fibonacci', 'fibonacci sequence'),
    'prime': ('prime', 'primality'),
    'palindrome': ('palindrome',),
    'sum': ('sum of n', 'sum from', 'sum first', 'sum natural', 'sum of first'),
    'even-odd': ('even or odd', 'even odd', 'check even', 'check if.*even'),
    'gcd': ('gcd', 'greatest common divisor', 'greatest common factor'),
    'lcm': ('lcm', 'least common multiple'),
    'reverse': ('reverse',),
    'armstrong': ('armstrong',),
    'table': ('multiplication table', 'times table'),
    'maximum': ('maximum', 'largest', 'max in', 'max of'),
    'sort': ('sort', 'sorting'),
    'temperature': ('celsius', 'fahrenheit', 'temperature'),
    'calculator': ('calculator', 'calculate', 'arithmetic'),
}
BUILTIN_IDS = {program['id'] for program in PROGRAMS}
run_lock = threading.Lock()
active_thread = None
active_processes = []
current_run_dir = None
run_paused = False
stop_requested = threading.Event()


def agent_template():
    defaults = {
        'Orchestrator': ('READY', 'Waiting for a program request'),
        'Generator': ('IDLE', 'Waiting for FIFO request'),
        'Executor': ('IDLE', 'Waiting for generated source'),
        'Reviewer': ('IDLE', 'Waiting for execution result'),
        'Tester': ('IDLE', 'Waiting for review result'),
        'Monitor': ('READY', 'Monitoring runtime'),
    }
    return [{
        'name': name,
        'role': AGENT_ROLES[name],
        'pid': '—',
        'status': defaults[name][0],
        'task': defaults[name][1],
        'ipcReceived': '—',
        'ipcSent': '—',
        'details': [],
    } for name in AGENT_ROLES]


def default_status():
    return {
        'systemOnline': False,
        'phase': 'READY',
        'currentRunId': '—',
        'selectedProgram': '',
        'request': '',
        'workspace': str(WORKSPACE),
        'currentStage': 'READY',
        'latestComponent': 'Orchestrator',
        'lockState': 'UNLOCKED',
        'backupPath': '—',
        'compilerCommand': '—',
        'compileStatus': '—',
        'programOutput': '',
        'expectedOutput': '',
        'actualOutput': '',
        'testResult': '—',
        'reviewResult': '—',
        'events': [],
        'agents': agent_template(),
        'ipc': [],
        'runHistory': [],
        'lastUpdated': time.strftime('%Y-%m-%dT%H:%M:%S'),
    }


def update_run_state(run_dir, changes=None, agent=None, event=None):
    state_path = Path(run_dir) / 'status.json'
    lock_path = Path(run_dir) / 'status.lock'
    with lock_path.open('a', encoding='utf-8') as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        state = json.loads(state_path.read_text(encoding='utf-8'))
        if changes:
            state.update(changes)
        if agent:
            agents = {item['name']: item for item in state.get('agents', [])}
            current = agents[agent['name']]
            current.update(agent)
            state['agents'] = list(agents.values())
            state['latestComponent'] = agent['name']
        if event:
            events = state.setdefault('events', [])
            events.append({
                'timestamp': time.strftime('%Y-%m-%dT%H:%M:%S'),
                'component': event['component'],
                'event': event['event'],
                'details': event.get('details', ''),
                'channel': event.get('channel', ''),
            })
            state['events'] = events[-160:]
            if event.get('channel'):
                state.setdefault('ipc', []).append({
                    'channel': event['channel'],
                    'sender': event.get('component', ''),
                    'receiver': event.get('receiver', ''),
                    'message': event.get('message', event.get('event', '')),
                    'payload': event.get('details', ''),
                    'status': 'SENT',
                    'timestamp': time.strftime('%Y-%m-%dT%H:%M:%S'),
                })
        state['lastUpdated'] = time.strftime('%Y-%m-%dT%H:%M:%S')
        temp_path = state_path.with_name(f'status.{os.getpid()}.tmp')
        temp_path.write_text(json.dumps(state, indent=2), encoding='utf-8')
        os.replace(temp_path, state_path)
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
    return state


def load_status():
    if CURRENT_RUN_PATH.exists():
        pointer = json.loads(CURRENT_RUN_PATH.read_text(encoding='utf-8'))
        status_path = Path(pointer['runDir']) / 'status.json'
        if status_path.exists():
            return json.loads(status_path.read_text(encoding='utf-8'))
    return default_status()


def record_process_telemetry(run_dir):
    run_dir = Path(run_dir)
    state = json.loads((run_dir / 'status.json').read_text(encoding='utf-8'))
    monitored = []
    for agent in state.get('agents', []):
        pid = agent.get('pid')
        if not isinstance(pid, int):
            continue
        try:
            stat_text = Path(f'/proc/{pid}/stat').read_text(encoding='utf-8')
            status_text = Path(f'/proc/{pid}/status').read_text(encoding='utf-8')
        except FileNotFoundError:
            if agent.get('memoryKb') is not None and agent.get('cpuTicks') is not None:
                monitored.append(
                    f'{agent["name"]} pid={pid} state=EXITED '
                    f'last_memory={agent["memoryKb"] / 1024:.1f}MB '
                    f'last_cpu_ticks={agent["cpuTicks"]}'
                )
            continue
        stat_fields = stat_text[stat_text.rfind(')') + 2:].split()
        process_start_ticks = int(stat_fields[19])
        if agent.get('processStartTicks') not in (None, process_start_ticks):
            if agent.get('memoryKb') is not None and agent.get('cpuTicks') is not None:
                monitored.append(
                    f'{agent["name"]} pid={pid} state=EXITED '
                    f'last_memory={agent["memoryKb"] / 1024:.1f}MB '
                    f'last_cpu_ticks={agent["cpuTicks"]}'
                )
            continue
        cpu_ticks = int(stat_fields[11]) + int(stat_fields[12])
        rss_line = next((line for line in status_text.splitlines() if line.startswith('VmRSS:')), None)
        rss_kb = int(rss_line.split()[1]) if rss_line else 0
        process_state = stat_fields[0]
        sample = {
            'name': agent['name'],
            'pid': pid,
            'memoryKb': rss_kb,
            'cpuTicks': cpu_ticks,
            'processState': process_state,
            'processStartTicks': process_start_ticks,
        }
        update_run_state(run_dir, agent=sample)
        monitored.append(
            f'{agent["name"]} pid={pid} state={process_state} '
            f'memory={rss_kb / 1024:.1f}MB cpu_ticks={cpu_ticks}'
        )
        update_run_state(run_dir, event={
            'component': 'Monitor',
            'event': 'TELEMETRY',
            'details': monitored[-1],
        })
    if monitored:
        monitor = next((agent for agent in state.get('agents', []) if agent['name'] == 'Monitor'), None)
        if monitor and monitor.get('pid') != os.getpid():
            update_run_state(run_dir, agent={
                'name': 'Monitor',
                'pid': monitor['pid'],
                'details': [
                    'Worker resources (live for active workers; last sample retained after exit):',
                    *monitored,
                ],
            })


def write_fifo(path, payload):
    with open(path, 'w', encoding='utf-8') as fifo:
        fifo.write(json.dumps(payload) + '\n')


def read_fifo(path):
    with open(path, 'r', encoding='utf-8') as fifo:
        line = fifo.readline()
    if not line:
        raise RuntimeError(f'FIFO closed without a payload: {path}')
    return json.loads(line)


def set_agent(run_dir, name, status, task, received=None, sent=None, details=None):
    agent = {
        'name': name,
        'status': status,
        'task': task,
        'pid': os.getpid(),
    }
    if received is not None:
        agent['ipcReceived'] = received
    if sent is not None:
        agent['ipcSent'] = sent
    if details is not None:
        agent['details'] = details
    update_run_state(run_dir, agent=agent)


def worker_process(role, run_dir):
    run_dir = Path(run_dir)
    manifest = json.loads((run_dir / 'manifest.json').read_text(encoding='utf-8'))
    role_entry = next(item for item in PIPELINE if item[0] == role)
    incoming = run_dir / 'fifos' / role_entry[1]
    outgoing = run_dir / 'fifos' / role_entry[2] if role_entry[2] else None
    if role == 'Monitor':
        set_agent(run_dir, role, 'ACTIVE', 'Monitoring agent processes and run health', details=[
            'Monitor process started.',
            f'Watching run workspace: {run_dir}',
        ])
        update_run_state(run_dir, event={
            'component': role,
            'event': 'monitor_started',
            'details': 'Monitoring all worker PIDs and FIFO pipeline state.',
        })
    payload = read_fifo(incoming)
    update_run_state(run_dir, event={
        'component': role,
        'event': 'IPC_RECEIVED',
        'details': f'Received task through {role_entry[1]}',
        'channel': role_entry[1],
    })

    try:
        if role == 'Generator':
            set_agent(run_dir, role, 'GENERATING', 'Request received; preparing the C source', payload['request'], 'SOURCE_READY', [
                f'Received request through {role_entry[1]}',
                'Preparing the selected program source.',
            ])
            time.sleep(STEP_VISIBILITY_SECONDS)
            request = manifest['request']
            source = manifest['source']
            source_path = run_dir / 'program.c'
            source_path.write_text(source, encoding='utf-8')
            next_payload = {
                'request': request,
                'sourcePath': str(source_path),
                'expectedOutput': manifest['expectedOutput'],
                'programName': manifest['programName'],
            }
            set_agent(run_dir, role, 'GENERATED', f'Generated {source_path.name} from the request', payload['request'], 'SOURCE_READY', [
                f'Received request through {role_entry[1]}',
                f'Generated source saved: {source_path}',
                f'Source size: {len(source.encode("utf-8"))} bytes',
            ])
            update_run_state(run_dir, changes={'sourceCode': source, 'sourcePath': str(source_path)}, event={
                'component': role, 'event': 'source_generated', 'details': f'Saved {source_path.name}',
                'channel': role_entry[2], 'receiver': 'Executor', 'message': 'SOURCE_READY',
            })
        elif role == 'Executor':
            source_path = Path(payload['sourcePath'])
            binary_path = run_dir / 'program'
            command = ['gcc', '-std=c11', '-Wall', '-Wextra', '-O2', str(source_path), '-o', str(binary_path)]
            command_text = ' '.join(command)
            update_run_state(run_dir, changes={'compilerCommand': command_text})
            set_agent(run_dir, role, 'COMPILING', f'Compiling {source_path.name} with GCC', 'SOURCE_READY', 'EXECUTION_RESULT', [
                f'Received {source_path.name} through {role_entry[1]}',
                f'Command: {command_text}',
            ])
            update_run_state(run_dir, event={
                'component': role, 'event': 'COMPILE_STARTED',
                'details': f'Running {command_text}',
            })
            time.sleep(STEP_VISIBILITY_SECONDS)
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=20, cwd=run_dir)
            compile_output = (compiled.stdout + compiled.stderr).strip()
            if compiled.returncode != 0:
                update_run_state(run_dir, event={
                    'component': role, 'event': 'COMPILE_FAILED',
                    'details': f'GCC exited with code {compiled.returncode}',
                })
                actual = f'Compile failed (exit {compiled.returncode})'
                if compile_output:
                    actual += f'\n{compile_output}'
                next_payload = {
                    'sourcePath': str(source_path), 'expectedOutput': payload['expectedOutput'],
                    'actualOutput': actual, 'compileStatus': 'FAILED',
                    'compileOutput': compile_output, 'returnCode': compiled.returncode,
                }
                execution_status = 'FAILED'
                update_run_state(run_dir, changes={
                    'compileStatus': 'FAILED', 'compileOutput': compile_output,
                    'programOutput': actual, 'actualOutput': actual,
                })
            else:
                update_run_state(run_dir, event={
                    'component': role, 'event': 'COMPILE_SUCCESS',
                    'details': 'GCC created the executable.',
                })
                try:
                    execution = subprocess.run(
                        [str(binary_path)], capture_output=True, text=True, timeout=5,
                        cwd=run_dir,
                    )
                    actual = execution.stdout
                    if execution.stderr:
                        actual += execution.stderr
                    next_payload = {
                        'sourcePath': str(source_path), 'expectedOutput': payload['expectedOutput'],
                        'actualOutput': actual, 'compileStatus': 'COMPILED',
                        'compileOutput': compile_output, 'returnCode': execution.returncode,
                    }
                    execution_status = 'COMPLETED' if execution.returncode == 0 else 'FAILED'
                    update_run_state(run_dir, changes={
                        'compileStatus': 'COMPILED',
                        'compileOutput': compile_output or 'No compiler warnings or errors.',
                        'programOutput': actual or '(no output)',
                        'actualOutput': actual,
                    })
                except subprocess.TimeoutExpired:
                    next_payload = {
                        'sourcePath': str(source_path), 'expectedOutput': payload['expectedOutput'],
                        'actualOutput': 'Program timed out after 5 seconds.',
                        'compileStatus': 'COMPILED', 'compileOutput': compile_output,
                        'returnCode': -1,
                    }
                    execution_status = 'TIMED OUT'
                    update_run_state(run_dir, changes={
                        'programOutput': next_payload['actualOutput'],
                        'actualOutput': next_payload['actualOutput'],
                    })
            update_run_state(run_dir, changes={'executorStatus': execution_status})
            executor_state = execution_status
            set_agent(run_dir, role, executor_state, 'Compiled and executed C program', 'SOURCE_READY', 'EXECUTION_RESULT', [
                f'Compiler: {next_payload["compileStatus"]}',
                f'Program exit code: {next_payload["returnCode"]}',
                f'Captured output: {next_payload["actualOutput"][:300]}',
            ])
            update_run_state(run_dir, event={
                'component': role, 'event': 'execution_finished',
                'details': f'GCC {next_payload["compileStatus"].lower()}; exit {next_payload["returnCode"]}',
                'channel': role_entry[2], 'receiver': 'Reviewer', 'message': 'EXECUTION_RESULT',
            })
        elif role == 'Reviewer':
            source_path = Path(payload['sourcePath'])
            backup_path = run_dir / 'backups' / 'program.c.bak'
            lock_path = run_dir / 'program.c.lock'
            with lock_path.open('w', encoding='utf-8') as lock_file:
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
                update_run_state(run_dir, changes={'lockState': 'LOCKED'}, event={
                    'component': role, 'event': 'LOCK_ACQUIRED',
                    'details': 'Exclusive source-file lock acquired.',
                })
                set_agent(run_dir, role, 'REVIEWING', 'Reviewing source under exclusive file lock', 'EXECUTION_RESULT', 'REVIEW_RESULT', [
                    'FIFO execution result received',
                    'Exclusive flock(LOCK_EX) acquired on program.c.lock',
                    'Creating source backup before review',
                ])
                time.sleep(STEP_VISIBILITY_SECONDS)
                backup_path.parent.mkdir(parents=True, exist_ok=True)
                backup_path.write_bytes(source_path.read_bytes())
                update_run_state(run_dir, event={
                    'component': role, 'event': 'BACKUP_CREATED',
                    'details': f'Source backup saved to {backup_path}',
                })
                source = source_path.read_text(encoding='utf-8')
                approved = bool(source.strip()) and payload['compileStatus'] == 'COMPILED' and payload['returnCode'] == 0
                decision = 'APPROVED' if approved else 'REJECTED'
                review_details = [
                    f'Backup created: {backup_path}',
                    f'Source review: {"non-empty and readable" if source.strip() else "source is empty"}',
                    f'Build review: {payload["compileStatus"]}',
                    f'Program exit code: {payload["returnCode"]}',
                    f'Decision: {decision}',
                ]
                update_run_state(run_dir, changes={
                    'reviewResult': decision, 'backupPath': str(backup_path),
                    'reviewDetails': review_details,
                })
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
                update_run_state(run_dir, changes={'lockState': 'RELEASED'}, event={
                    'component': role, 'event': 'LOCK_RELEASED',
                    'details': 'Source review complete; exclusive lock released.',
                })
            next_payload = dict(payload)
            next_payload['reviewResult'] = decision
            set_agent(run_dir, role, decision, 'Reviewed source, backup, build, and exit status', 'EXECUTION_RESULT', 'REVIEW_RESULT', review_details + ['Exclusive lock released'])
            update_run_state(run_dir, event={
                'component': role, 'event': 'review_finished',
                'details': f'{decision}; backup created; file lock released',
                'channel': role_entry[2], 'receiver': 'Tester', 'message': 'REVIEW_RESULT',
            })
        elif role == 'Tester':
            expected = payload['expectedOutput']
            actual = payload['actualOutput']
            set_agent(run_dir, role, 'TESTING', 'Comparing expected output with program output', 'REVIEW_RESULT', 'TEST_RESULT', [
                'Review result received from Reviewer.',
                'Comparing expected and actual output.',
            ])
            update_run_state(run_dir, event={
                'component': role, 'event': 'TEST_STARTED',
                'details': 'Comparing expected output with program output.',
            })
            time.sleep(STEP_VISIBILITY_SECONDS)
            passed = (
                payload['reviewResult'] == 'APPROVED'
                and payload['compileStatus'] == 'COMPILED'
                and payload['returnCode'] == 0
                and actual.strip() == expected.strip()
            )
            result = 'PASS — expected output matches actual output' if passed else 'FAIL — expected output does not match actual output'
            next_payload = dict(payload)
            next_payload['testResult'] = result
            set_agent(run_dir, role, 'PASS' if passed else 'FAIL', 'Comparing expected output with program output', 'REVIEW_RESULT', 'TEST_RESULT', [
                f'Expected: {expected.strip()!r}',
                f'Actual: {actual.strip()!r}',
                result,
            ])
            update_run_state(run_dir, changes={
                'expectedOutput': expected, 'actualOutput': actual,
                'testResult': result, 'phase': 'TESTING',
            }, event={
                'component': role, 'event': 'output_compared',
                'details': result, 'channel': role_entry[2], 'receiver': 'Monitor', 'message': 'TEST_RESULT',
            })
        else:
            final_phase = 'COMPLETE' if payload['testResult'].startswith('PASS') else 'FAILED'
            details = [
                f'Run result: {final_phase}',
                f'Run workspace: {run_dir}',
            ]
            current_state = json.loads((run_dir / 'status.json').read_text(encoding='utf-8'))
            monitor = next(item for item in current_state['agents'] if item['name'] == 'Monitor')
            details.extend(monitor.get('details', []))
            set_agent(
                run_dir, role,
                'HEALTHY' if final_phase == 'COMPLETE' else 'ATTENTION',
                'Recording run health and final result', 'TEST_RESULT', 'MONITORING_COMPLETE', details,
            )
            update_run_state(run_dir, changes={
                'phase': final_phase, 'systemOnline': True,
                'currentStage': 'COMPLETE' if final_phase == 'COMPLETE' else 'FAILED',
                'monitorStatus': 'HEALTHY' if final_phase == 'COMPLETE' else 'ATTENTION',
            }, event={
                'component': role, 'event': 'run_monitored',
                'details': f'{final_phase}; run artifacts and status recorded',
                'channel': role_entry[1], 'receiver': role,
            })
            return

        update_run_state(run_dir, changes={'phase': role.upper(), 'currentStage': role.upper()})
        write_fifo(outgoing, next_payload)
    except Exception as error:
        set_agent(run_dir, role, 'FAILED', str(error), received='—', details=[f'{type(error).__name__}: {error}'])
        update_run_state(run_dir, changes={'phase': 'FAILED', 'error': f'{role}: {error}'}, event={
            'component': role, 'event': 'agent_failed', 'details': f'{type(error).__name__}: {error}',
        })
        raise


def _agent_entry(arguments):
    if len(arguments) != 4 or arguments[1] != '--agent-worker':
        raise ValueError('Invalid worker invocation.')
    worker_process(arguments[2], arguments[3])


def parse_numbers(request):
    return [int(value) for value in re.findall(r'(?<![A-Za-z])[-+]?\d+', request)]


def make_builtin_program(program_id, request):
    numbers = parse_numbers(request)
    first = numbers[0] if numbers else 5
    second = numbers[1] if len(numbers) > 1 else 3
    if program_id == 'factorial':
        if not 0 <= first <= 20:
            raise ValueError('Factorial input must be between 0 and 20.')
        expected = f'factorial({first}) = {__import__("math").factorial(first)}\n'
        source = f'''#include <stdio.h>
int main(void) {{
    unsigned long long result = 1;
    for (int i = 2; i <= {first}; ++i) result *= (unsigned long long)i;
    printf("factorial({first}) = %llu\\n", result);
    return 0;
}}
'''
    elif program_id == 'fibonacci':
        count = first if numbers else 10
        if not 0 <= count <= 50:
            raise ValueError('Choose at most 50 Fibonacci numbers.')
        values = []
        a, b = 0, 1
        for _ in range(count):
            values.append(a)
            a, b = b, a + b
        expected = ' '.join(str(value) for value in values) + '\n'
        source = f'''#include <stdio.h>
int main(void) {{
    unsigned long long a = 0, b = 1;
    for (int i = 0; i < {count}; ++i) {{
        printf("%s%llu", i ? " " : "", a);
        unsigned long long next = a + b;
        a = b;
        b = next;
    }}
    putchar('\\n');
    return 0;
}}
'''
    elif program_id == 'prime':
        value = first
        if abs(value) > 2_147_483_647:
            raise ValueError('Prime checks support signed 32-bit integers.')
        import math
        is_prime = value >= 2 and all(value % divisor for divisor in range(2, math.isqrt(value) + 1))
        expected = f'{value} is {"prime" if is_prime else "not prime"}\n'
        source = f'''#include <stdio.h>
int main(void) {{
    long long value = {value};
    int prime = value >= 2;
    for (long long divisor = 2; prime && divisor <= value / divisor; ++divisor)
        if (value % divisor == 0) {{ prime = 0; break; }}
    printf("%lld is %sprime\\n", value, prime ? "" : "not ");
    return 0;
}}
'''
    elif program_id == 'palindrome':
        value = abs(first)
        if value > 2_147_483_647:
            raise ValueError('Palindrome checks support signed 32-bit integers.')
        expected = f'{value} is {"a palindrome" if str(value) == str(value)[::-1] else "not a palindrome"}\n'
        source = f'''#include <stdio.h>
int main(void) {{
    long long value = {value}, original = value, reversed = 0;
    do {{ reversed = reversed * 10 + value % 10; value /= 10; }} while (value > 0);
    printf("%lld is %sa palindrome\\n", original, original == reversed ? "" : "not ");
    return 0;
}}
'''
    elif program_id == 'sum':
        value = numbers[-1] if numbers else first
        if not 0 <= value <= 1_000_000:
            raise ValueError('Sum input must be between 0 and 1,000,000.')
        expected = f'sum 1..{value} = {value * (value + 1) // 2}\n'
        source = f'''#include <stdio.h>
int main(void) {{
    long long sum = (long long){value} * ({value} + 1) / 2;
    printf("sum 1..{value} = %lld\\n", sum);
    return 0;
}}
'''
    elif program_id == 'even-odd':
        if abs(first) > 2_147_483_647:
            raise ValueError('Even/odd checks support signed 32-bit integers.')
        expected = f'{first} is {"even" if first % 2 == 0 else "odd"}\n'
        source = f'''#include <stdio.h>
int main(void) {{
    int value = {first};
    printf("%d is %s\\n", value, value % 2 == 0 ? "even" : "odd");
    return 0;
}}
'''
    elif program_id in ('gcd', 'lcm'):
        import math
        left, right = first, second
        if any(abs(value) > 1_000_000_000 for value in (left, right)):
            raise ValueError('GCD and LCM inputs must be between -1,000,000,000 and 1,000,000,000.')
        gcd = math.gcd(left, right)
        if program_id == 'gcd':
            expected = f'gcd({left}, {right}) = {gcd}\n'
            result_expr = f'{gcd}'
            label = 'gcd'
        else:
            result = abs(left * right) // gcd if gcd else 0
            expected = f'lcm({left}, {right}) = {result}\n'
            result_expr = f'{result}'
            label = 'lcm'
        source = f'''#include <stdio.h>
int main(void) {{
    int left = {left}, right = {right};
    while (right != 0) {{ int remainder = left % right; left = right; right = remainder; }}
    long long answer = {result_expr};
    printf("{label}({first}, {second}) = %lld\\n", answer);
    return 0;
}}
'''
    elif program_id == 'reverse':
        value = abs(first)
        if value > 2_147_483_647:
            raise ValueError('Reverse-number input must fit in a signed 32-bit integer.')
        reversed_value = int(str(value)[::-1])
        expected = f'reverse({value}) = {reversed_value}\n'
        source = f'''#include <stdio.h>
int main(void) {{
    long long value = {value}, reversed = 0;
    do {{ reversed = reversed * 10 + value % 10; value /= 10; }} while (value > 0);
    printf("reverse({abs(first)}) = %lld\\n", reversed);
    return 0;
}}
'''
    elif program_id == 'armstrong':
        value = abs(first)
        if value > 2_147_483_647:
            raise ValueError('Armstrong-number input must fit in a signed 32-bit integer.')
        digits = str(value)
        expected = f'{value} is {"an Armstrong number" if sum(int(digit) ** len(digits) for digit in digits) == value else "not an Armstrong number"}\n'
        source = f'''#include <stdio.h>
int main(void) {{
    long long value = {value}, original = value, total = 0, digits = 0, temp = value;
    do {{ ++digits; temp /= 10; }} while (temp > 0);
    temp = value;
    do {{ long long digit = temp % 10, power = 1; for (int i = 0; i < digits; ++i) power *= digit; total += power; temp /= 10; }} while (temp > 0);
    printf("%lld is %san Armstrong number\\n", original, total == original ? "" : "not ");
    return 0;
}}
'''
    elif program_id == 'table':
        value = first
        expected = ''.join(f'{value} x {i} = {value * i}\n' for i in range(1, 11))
        source = f'''#include <stdio.h>
int main(void) {{
    for (int i = 1; i <= 10; ++i) printf("{value} x %d = %lld\\n", i, (long long){value} * i);
    return 0;
}}
'''
    elif program_id in ('maximum', 'sort'):
        values = numbers if numbers else [8, 3, 5, 1, 9]
        if len(values) > 25 or any(abs(value) > 100_000 for value in values):
            raise ValueError('Array examples support up to 25 integers between -100,000 and 100,000.')
        values_initializer = ', '.join(str(value) for value in values)
        value_count = len(values)
        if program_id == 'maximum':
            expected = f'maximum = {max(values)}\n'
            source = f'''#include <stdio.h>
int main(void) {{
    int values[] = {{{values_initializer}}}, maximum = values[0];
    for (int i = 1; i < {value_count}; ++i) if (values[i] > maximum) maximum = values[i];
    printf("maximum = %d\\n", maximum);
    return 0;
}}
'''
        else:
            ordered = sorted(values)
            expected = 'sorted: ' + ' '.join(str(value) for value in ordered) + '\n'
            source = f'''#include <stdio.h>
int main(void) {{
    int values[] = {{{values_initializer}}};
    for (int i = 0; i < {value_count}; ++i)
        for (int j = i + 1; j < {value_count}; ++j)
            if (values[j] < values[i]) {{ int t = values[i]; values[i] = values[j]; values[j] = t; }}
    printf("sorted:");
    for (int i = 0; i < {value_count}; ++i) printf(" %d", values[i]);
    putchar('\\n');
    return 0;
}}
'''
    elif program_id == 'temperature':
        celsius = first
        fahrenheit = celsius * 9 / 5 + 32
        expected = f'{celsius} C = {fahrenheit:.2f} F\n'
        source = f'''#include <stdio.h>
int main(void) {{
    double celsius = {celsius}, fahrenheit = celsius * 9.0 / 5.0 + 32.0;
    printf("{celsius} C = %.2f F\\n", fahrenheit);
    return 0;
}}
'''
    else:
        match = re.search(r'(-?\d+)\s*([+\-*/])\s*(-?\d+)', request)
        left, operator, right = (int(match.group(1)), match.group(2), int(match.group(3))) if match else (8, '+', 2)
        if operator == '/' and right == 0:
            raise ValueError('Division by zero is not supported.')
        expression = {'+': left + right, '-': left - right, '*': left * right, '/': left / right}[operator]
        expected = f'{left} {operator} {right} = {expression:.2f}\n'
        source = f'''#include <stdio.h>
int main(void) {{
    double left = {left}, right = {right}, result = left {operator} right;
    printf("{left} {operator} {right} = %.2f\\n", result);
    return 0;
}}
'''
    return source, expected


def resolve_builtin(request):
    lowered = request.lower()
    for program_id, patterns in ALIASES.items():
        if any(re.search(pattern, lowered) for pattern in patterns):
            return program_id
    return None


def prepare_request(payload):
    request = str(payload.get('request', payload.get('program', ''))).strip()
    source = str(payload.get('source', '')).strip()
    expected = str(payload.get('expectedOutput', '')).strip()
    if not request:
        raise ValueError('Enter a program request before starting.')
    if len(request) > MAX_REQUEST_SIZE:
        raise ValueError(f'Program requests must be under {MAX_REQUEST_SIZE} characters.')
    if source:
        if len(source.encode('utf-8')) > MAX_SOURCE_SIZE:
            raise ValueError(f'C source must be under {MAX_SOURCE_SIZE} bytes.')
        if not expected:
            raise ValueError('Enter the expected output for custom C source so the Tester can compare it.')
        return request, request, source, expected + '\n'
    program_id = resolve_builtin(request)
    if program_id not in BUILTIN_IDS:
        raise ValueError('That request is not in the built-in catalog. Select a sample or provide custom C source.')
    program = next(item for item in PROGRAMS if item['id'] == program_id)
    generated_source, expected_output = make_builtin_program(program_id, request)
    return request, program['name'], generated_source, expected_output


def _run_pipeline(run_dir, request, program_name):
    global active_processes
    run_dir = Path(run_dir)
    fifo_dir = run_dir / 'fifos'
    initial_payload = {'request': request, 'programName': program_name}
    try:
        time.sleep(STEP_VISIBILITY_SECONDS)
        record_process_telemetry(run_dir)
        next_telemetry = time.monotonic() + 1
        write_fifo(fifo_dir / PIPELINE[0][1], initial_payload)
        deadline = time.monotonic() + 90
        while any(process.poll() is None for process in active_processes):
            if stop_requested.is_set():
                return
            if time.monotonic() > deadline:
                raise TimeoutError('The agent pipeline did not finish within 90 seconds.')
            failed = [process for process in active_processes if process.poll() not in (None, 0)]
            if failed:
                raise RuntimeError(f'An agent worker exited with code {failed[0].returncode}.')
            if time.monotonic() >= next_telemetry:
                record_process_telemetry(run_dir)
                next_telemetry = time.monotonic() + 1
            time.sleep(0.1)
        state = json.loads((run_dir / 'status.json').read_text(encoding='utf-8'))
        if state['phase'] not in ('COMPLETE', 'FAILED'):
            raise RuntimeError(f'Agent pipeline ended unexpectedly in phase {state["phase"]}.')
        history = state.get('runHistory', [])
        history.insert(0, {
            'runId': state['currentRunId'],
            'program': program_name,
            'status': state['phase'],
            'result': state.get('actualOutput', '').strip(),
        })
        final_phase = state['phase']
        update_run_state(run_dir, changes={'runHistory': history[:50]}, agent={
            'name': 'Orchestrator', 'status': 'COMPLETE' if final_phase == 'COMPLETE' else 'FAILED',
            'task': f'Pipeline finished: {final_phase}',
        })
    except Exception as error:
        if not stop_requested.is_set():
            update_run_state(run_dir, changes={
                'phase': 'FAILED', 'currentStage': 'FAILED', 'error': str(error),
                'systemOnline': False,
            }, agent={
                'name': 'Orchestrator', 'status': 'FAILED', 'task': str(error),
            }, event={'component': 'Orchestrator', 'event': 'pipeline_failed', 'details': str(error)})
    finally:
        if stop_requested.is_set():
            update_run_state(run_dir, changes={
                'phase': 'STOPPED', 'currentStage': 'STOPPED', 'systemOnline': False,
                'lockState': 'UNLOCKED',
            }, agent={
                'name': 'Orchestrator', 'status': 'STOPPED', 'task': 'Run stopped by user.',
            }, event={'component': 'Orchestrator', 'event': 'run_stopped', 'details': 'Run stopped by user.'})
        for process in active_processes:
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        active_processes = []


def start_run(payload):
    global active_thread, active_processes, current_run_dir, run_paused
    request, program_name, source, expected = prepare_request(payload)
    with run_lock:
        if active_thread and active_thread.is_alive():
            if load_status()['phase'] in ('COMPLETE', 'FAILED', 'STOPPED'):
                active_thread.join(timeout=1)
            if active_thread.is_alive():
                raise RuntimeError('A program run is already active. Stop it before starting another.')
        stop_requested.clear()
        run_paused = False
        run_id = f"AURA-{time.strftime('%Y%m%d-%H%M%S')}-{uuid.uuid4().hex[:6].upper()}"
        run_dir = WORKSPACE / 'runs' / run_id
        (run_dir / 'fifos').mkdir(parents=True, exist_ok=False)
        (run_dir / 'backups').mkdir()
        manifest = {
            'request': request, 'programName': program_name,
            'source': source, 'expectedOutput': expected,
        }
        (run_dir / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
        for _, input_fifo, _ in PIPELINE:
            os.mkfifo(run_dir / 'fifos' / input_fifo, 0o600)
        agents = agent_template()
        agents[0].update({'pid': os.getpid(), 'status': 'ROUTING', 'task': f'Routing request: {request}'})
        agents[1]['status'] = 'QUEUED'
        agents[2]['status'] = 'QUEUED'
        agents[3]['status'] = 'QUEUED'
        agents[4]['status'] = 'QUEUED'
        agents[5]['status'] = 'MONITORING'
        state = default_status()
        state.update({
            'systemOnline': True, 'phase': 'STARTING', 'currentStage': 'ORCHESTRATOR',
            'currentRunId': run_id, 'selectedProgram': program_name,
            'request': request, 'workspace': str(run_dir), 'agents': agents,
            'events': [{
                'timestamp': time.strftime('%Y-%m-%dT%H:%M:%S'), 'component': 'Orchestrator',
                'event': 'request_received', 'details': request, 'channel': '',
            }],
            'ipc': [], 'sourceCode': '', 'sourcePath': '—', 'compilerCommand': '—',
            'compileStatus': '—', 'programOutput': '', 'actualOutput': '',
            'expectedOutput': expected, 'reviewResult': 'PENDING',
            'testResult': 'PENDING', 'backupPath': '—', 'lockState': 'UNLOCKED',
            'runHistory': load_status().get('runHistory', []),
            'lastUpdated': time.strftime('%Y-%m-%dT%H:%M:%S'),
        })
        (run_dir / 'status.json').write_text(json.dumps(state, indent=2), encoding='utf-8')
        CURRENT_RUN_PATH.write_text(json.dumps({'runDir': str(run_dir)}, indent=2), encoding='utf-8')
        current_run_dir = run_dir
        active_processes = []
        for role, _, _ in reversed(PIPELINE):
            process = subprocess.Popen(
                [sys.executable, str(Path(__file__).resolve()), '--agent-worker', role, str(run_dir)],
                cwd=str(run_dir), stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            active_processes.append(process)
            update_run_state(run_dir, agent={
                'name': role, 'pid': process.pid,
                'status': 'MONITORING' if role == 'Monitor' else 'WAITING',
                'task': 'Monitoring pipeline and process health' if role == 'Monitor' else 'Waiting for FIFO message',
            }, event={
                'component': role, 'event': 'PROCESS_STARTED',
                'details': f'Worker process started with PID {process.pid}.',
            })
        update_run_state(run_dir, changes={'phase': 'RUNNING'}, agent={
            'name': 'Orchestrator', 'pid': os.getpid(), 'status': 'ROUTING',
            'task': f'Routing request: {request}', 'ipcSent': 'REQUEST',
        })
        update_run_state(run_dir, event={
            'component': 'Orchestrator', 'event': 'ipc_sent', 'details': request,
            'channel': 'orchestrator_to_generator.fifo', 'receiver': 'Generator',
        })
        active_thread = threading.Thread(
            target=_run_pipeline, args=(run_dir, request, program_name), daemon=True,
        )
        active_thread.start()
        return {'status': 'started', 'runId': run_id, 'program': program_name}


def set_pause():
    global run_paused
    with run_lock:
        if not active_thread or not active_thread.is_alive() or not active_processes:
            raise RuntimeError('There is no active program run to pause or resume.')
        signal_to_send = signal.SIGCONT if run_paused else signal.SIGSTOP
        for process in active_processes:
            if process.poll() is None:
                os.killpg(process.pid, signal_to_send)
        run_paused = not run_paused
        update_run_state(current_run_dir, changes={
            'phase': 'RUNNING' if not run_paused else 'PAUSED',
            'currentStage': 'RUNNING' if not run_paused else 'PAUSED',
        }, event={
            'component': 'Orchestrator',
            'event': 'run_resumed' if not run_paused else 'run_paused',
            'details': 'All active worker process groups resumed.' if not run_paused else 'All active worker process groups paused.',
        })
        return {'status': 'resumed' if not run_paused else 'paused'}


def stop_run():
    global active_thread, run_paused
    with run_lock:
        if not active_thread or not active_thread.is_alive():
            raise RuntimeError('There is no active program run to stop.')
        stop_requested.set()
        run_paused = False
        for process in active_processes:
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGCONT)
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        if current_run_dir:
            update_run_state(current_run_dir, changes={
                'phase': 'STOPPED', 'currentStage': 'STOPPED', 'systemOnline': False,
                'lockState': 'UNLOCKED',
            }, event={'component': 'Orchestrator', 'event': 'run_stopped', 'details': 'Run stopped by user.'})
        return {'status': 'stopping'}


def send_json(handler, payload, status=200):
    encoded = json.dumps(payload, ensure_ascii=False).encode('utf-8')
    handler.send_response(status)
    handler.send_header('Content-Type', 'application/json; charset=utf-8')
    handler.send_header('Content-Length', str(len(encoded)))
    handler.send_header('Cache-Control', 'no-store')
    handler.end_headers()
    handler.wfile.write(encoded)


class AuraHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/api/status':
            send_json(self, load_status())
            return
        if self.path == '/api/programs':
            send_json(self, {'programs': PROGRAMS})
            return
        if self.path in ('/', '/index.html'):
            file_path = ROOT / 'dashboard' / 'index.html'
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            body = file_path.read_bytes()
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path in ('/static/app.js', '/static/styles.css'):
            file_path = ROOT / 'dashboard' / self.path.rsplit('/', 1)[1]
            content_type = 'text/javascript; charset=utf-8' if file_path.suffix == '.js' else 'text/css; charset=utf-8'
            body = file_path.read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        send_json(self, {'error': 'Not found'}, 404)

    def do_POST(self):
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if length > MAX_SOURCE_SIZE + MAX_REQUEST_SIZE + 16_384:
                send_json(self, {'error': 'Request body exceeds the allowed size.'}, 413)
                return
            body = self.rfile.read(length).decode('utf-8') if length else '{}'
            payload = json.loads(body)
            if self.path in ('/api/start', '/api/deploy'):
                send_json(self, start_run(payload), 202)
                return
            if self.path == '/api/pause':
                send_json(self, set_pause())
                return
            if self.path == '/api/stop':
                send_json(self, stop_run())
                return
            send_json(self, {'error': 'Not found'}, 404)
        except (ValueError, RuntimeError, json.JSONDecodeError) as error:
            send_json(self, {'error': str(error)}, 400)
        except Exception as error:
            self.log_error('AURA request failed: %s', error)
            send_json(self, {'error': 'Internal AURA service error.'}, 500)

    def log_message(self, format_string, *args):
        return


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--agent-worker':
        _agent_entry(sys.argv)
        return
    WORKSPACE.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer((HOST, PORT), AuraHandler)
    print(f'AURA control center listening on http://{HOST}:{PORT}')
    server.serve_forever()


if __name__ == '__main__':
    main()
