"""Configure the detection service from Supervisor options and launch it."""
import json
import os
from pathlib import Path


def configure(options, data_dir=Path('/data')):
    values = {
        'confidence_threshold': (0.5, float, 0, 1),
        'inference_threads': (1, int, 1, 32),
        'max_image_size': (1024, int, 128, 4096),
        'face_detection_size': (960, int, 320, 1920),
    }
    parsed = {}
    for key, (default, kind, minimum, maximum) in values.items():
        value = options.get(key, default)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not minimum <= value <= maximum:
            raise ValueError('Invalid ' + key)
        if kind is int and int(value) != value:
            raise ValueError('Invalid ' + key)
        parsed[key] = kind(value)
    enabled = options.get('classification_enabled', True)
    if not isinstance(enabled, bool):
        raise ValueError('Invalid classification_enabled')
    (data_dir / 'detection').mkdir(parents=True, exist_ok=True)
    cache = data_dir / 'model-cache'
    cache.mkdir(parents=True, exist_ok=True)
    return {
        'DEFAULT_BACKEND': 'onnx',
        'AVAILABLE_BACKENDS': '["onnx"]',
        'ONNX_CONFIDENCE_THRESHOLD': str(parsed['confidence_threshold']),
        'ONNX_INTRA_OP_THREADS': str(parsed['inference_threads']),
        'ONNX_INTER_OP_THREADS': '1',
        'MAX_IMAGE_SIZE': str(parsed['max_image_size']),
        'FACE_DET_SIZE': str(parsed['face_detection_size']),
        'CLASSIFICATION_ENABLED': str(enabled).lower(),
        'FACE_DB_PATH': str(data_dir / 'detection' / 'faces.db'),
        'HOME': str(cache),
        'HF_HOME': str(cache / 'huggingface'),
        'XDG_CACHE_HOME': str(cache),
    }


if __name__ == '__main__':
    options = Path('/data/options.json')
    os.environ.update(configure(json.loads(options.read_text()) if options.exists() else {}))
    os.chdir('/app')
    os.execvp('uvicorn', ['uvicorn', 'main:app', '--host', '0.0.0.0', '--port', '8000'])
