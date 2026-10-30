"""JSON 协议适配层；评分始终调用未修改的官方 Python，供基准脚本使用。"""
from official_reference import *


def main():
    try:
        request = json.load(sys.stdin)
        if request['action'] != 'scene_b':
            raise ValueError('official_runner only supports scene_b')
        result = evaluate_scene_b(request['graph'], request['plan'], request['bandwidth'],
                                  request['capacity'], request['delay'])
        print(json.dumps(normalized(result)))
    except Exception as error:
        print(json.dumps({'error': str(error), 'error_type': type(error).__name__}))
        raise SystemExit(1)


if __name__ == '__main__':
    main()
