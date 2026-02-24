# SPDX-License-Identifier: MIT
# Copyright © 2024-2026 Intel Corporation

import json
import logging
import queue
import socket
import threading
import time
import typing

logger = logging.getLogger('QmpMonitor')


class QmpMonitor():
    def __init__(self, socket_path: str, socket_timeout: int) -> None:
        self.sockpath = socket_path
        self.timeout = socket_timeout
        self.sock: socket.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.sockpath)
        self.sockf: typing.TextIO = self.sock.makefile(mode='rw', errors='strict')
        self.qmp_queue: queue.Queue = queue.Queue()
        self.monitor_thread: threading.Thread = threading.Thread(target=self.__queue_qmp_output,
                                                                 args=(self.sockf, self.qmp_queue),
                                                                 daemon=True)
        self.monitor_thread.start()
        # It is required to enable capabilities before using QMP
        self.__enable_qmp_capabilities()

    def __enable_qmp_capabilities(self) -> None:
        json.dump({'execute': 'qmp_capabilities'}, self.sockf)
        self.sockf.flush()

    def __queue_qmp_output(self, out: typing.TextIO, q: queue.Queue) -> None:
        for line in iter(out.readline, ''):
            logger.debug('[QMP RSP] <- %s', line)
            qmp_msg = json.loads(line)
            q.put(qmp_msg)

    @property
    def monitor_queue(self) -> queue.Queue:
        return self.qmp_queue

    def query_status(self) -> str:
        json.dump({'execute': 'query-status'}, self.sockf)
        self.sockf.flush()

        ret: typing.Dict = {}
        while 'status' not in ret:
            qmp_msg = self.qmp_queue.get()
            if 'return' in qmp_msg:
                ret = qmp_msg.get('return')

        status: str = ret['status']
        logger.debug('Machine status: %s', status)
        return status

    def query_jobs(self, requested_type: str) -> typing.Tuple[str, str]:
        json.dump({'execute': 'query-jobs'}, self.sockf)
        self.sockf.flush()

        job_type: str = ''
        job_status: str = ''
        job_error: str = ''
        ret: typing.Dict = {}

        qmp_msg = self.qmp_queue.get()
        # logger.debug('[QMP RSP Queue] -> %s', qmp_msg)
        if 'return' in qmp_msg:
            ret = qmp_msg.get('return')
            for param in ret:
                job_type = param.get('type')
                job_status = param.get('status')
                job_error = param.get('error')

                if job_type == requested_type:
                    break

        return (job_status, job_error)

    def get_qmp_event(self) -> str:
        qmp_msg = self.qmp_queue.get()
        # logger.debug('[QMP RSP Queue] -> %s', qmp_msg)
        event: str = qmp_msg.get('event', '')
        return event

    def get_qmp_event_job(self) -> str:
        qmp_msg = self.qmp_queue.get()
        # logger.debug('[QMP RSP Queue] -> %s', qmp_msg)

        status: str = ''
        if qmp_msg.get('event') == 'JOB_STATUS_CHANGE':
            status = qmp_msg.get('data', {}).get('status', '')

        return status

    def system_reset(self) -> None:
        json.dump({'execute': 'system_reset'}, self.sockf)
        self.sockf.flush()

    def system_wakeup(self) -> None:
        json.dump({'execute': 'system_wakeup'}, self.sockf)
        self.sockf.flush()

    def stop(self) -> None:
        json.dump({'execute': 'stop'}, self.sockf)
        self.sockf.flush()

    def cont(self) -> None:
        json.dump({'execute': 'cont'}, self.sockf)
        self.sockf.flush()

    def quit(self) -> None:
        json.dump({'execute': 'quit'}, self.sockf)
        self.sockf.flush()

    def __query_snapshot(self) -> typing.Tuple[str, str]:
        json.dump({'execute': 'query-named-block-nodes'}, self.sockf)
        self.sockf.flush()

        node_name: str = ''
        snapshot_tag: str = ''
        ret: typing.Dict = {}

        qmp_msg = self.qmp_queue.get()
        while 'return' not in qmp_msg:
            qmp_msg = self.qmp_queue.get()

        ret = qmp_msg.get('return')
        for block in ret:
            if block.get('drv') == 'qcow2' and block.get('ro') is False:
                node_name = block.get('node-name')
                # Get the most recent state snapshot from the snapshots list:
                snapshots = block.get('image').get('snapshots')
                if snapshots:
                    snapshot_tag = snapshots[-1].get('name')
                break

        return (node_name, snapshot_tag)

    def save_snapshot(self) -> None:
        job_id: str = f'savevm_{time.time()}'
        snapshot_tag = f'vm_state_{time.time()}'
        node_name, _ = self.__query_snapshot()
        logger.debug('[QMP snapshot-save] snapshot_tag: %s, block device node: %s', snapshot_tag, node_name)

        # Note: command 'snapshot-save' is supported since QEMU 6.0
        json.dump({'execute': 'snapshot-save',
            'arguments': {'job-id': job_id, 'tag': snapshot_tag, 'vmstate': node_name, 'devices': [node_name]}},
            self.sockf)
        self.sockf.flush()

    def load_snapshot(self) -> None:
        job_id: str = f'loadvm_{time.time()}'
        node_name, snapshot_tag = self.__query_snapshot()
        logger.debug('[QMP snapshot-load] snapshot_tag: %s, block device node: %s', snapshot_tag, node_name)

        # Note: command 'snapshot-load' is supported since QEMU 6.0
        json.dump({'execute': 'snapshot-load',
            'arguments': {'job-id': job_id, 'tag': snapshot_tag, 'vmstate': node_name, 'devices': [node_name]}},
            self.sockf)
        self.sockf.flush()
