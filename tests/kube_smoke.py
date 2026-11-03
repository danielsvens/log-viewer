"""Opt-in live test: KUBE_SMOKE=1 KUBE_SMOKE_NAMESPACE=test KUBE_SMOKE_IMAGE=... python3 tests/kube_smoke.py.

Requires a disposable namespace, a compatible image with sh/sleep, and workload
creation/deletion permissions in addition to the viewer's normal read permissions.
"""
import json
import os
import subprocess
import uuid

if os.environ.get('KUBE_SMOKE') != '1':
    raise SystemExit('Set KUBE_SMOKE=1 to create a disposable deployment.')
namespace = os.environ['KUBE_SMOKE_NAMESPACE']
image = os.environ['KUBE_SMOKE_IMAGE']
context = os.environ.get('KUBE_SMOKE_CONTEXT')
base = ['kubectl'] + (['--context', context] if context else []) + ['--namespace', namespace]
name = 'lv-smoke-' + uuid.uuid4().hex[:10]
manifest = {'apiVersion': 'apps/v1', 'kind': 'Deployment', 'metadata': {'name': name},
            'spec': {'replicas': 2, 'selector': {'matchLabels': {'app': name}},
                     'template': {'metadata': {'labels': {'app': name}}, 'spec': {'containers': [
                         {'name': c, 'image': image, 'command': ['sh', '-c', f'echo {name}-{c}; sleep 120']}
                         for c in ('app', 'sidecar')]}}}}
try:
    subprocess.run(base + ['create', '-f', '-'], input=json.dumps(manifest), text=True, check=True, timeout=30)
    subprocess.run(base + ['rollout', 'status', 'deployment/' + name, '--timeout=90s'], check=True, timeout=100)
    args = [os.path.abspath(os.environ.get('LOG_VIEWER_BIN', 'log-viewer')), '--dump', '--kube', 'deployment/' + name,
            '--kube-namespace', namespace]
    if context: args += ['--kube-context', context]
    result = subprocess.run(args, text=True, capture_output=True, check=True, timeout=30)
    for c in ('app', 'sidecar'):
        assert result.stdout.count(f'{name}-{c}') == 2, result.stdout
finally:
    subprocess.run(base + ['delete', 'deployment', name, '--ignore-not-found', '--wait=false'], check=True, timeout=30)
print('Live Kubernetes multi-replica and multi-container history passed.')
