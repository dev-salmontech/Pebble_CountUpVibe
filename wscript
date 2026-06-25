import os.path

top = '.'
out = 'build'


def options(ctx):
    ctx.load('pebble_sdk')


def configure(ctx):
    ctx.load('pebble_sdk')


def build(ctx):
    binaries = []
    build_worker = os.path.exists('worker_src')

    for platform in ctx.env.TARGET_PLATFORMS:
        ctx.set_env(ctx.all_envs[platform])
        ctx.set_group(ctx.env.PLATFORM_NAME)

        app_elf = '{}/pebble-countupvibe.elf'.format(ctx.env.BUILD_DIR)
        ctx.pbl_program(
            source=ctx.path.ant_glob('src/**/*.c'),
            target=app_elf
        )

        binary = {'platform': platform, 'app_elf': app_elf}
        if build_worker:
            worker_elf = '{}/pebble-worker.elf'.format(ctx.env.BUILD_DIR)
            ctx.pbl_worker(
                source=ctx.path.ant_glob('worker_src/**/*.c'),
                target=worker_elf
            )
            binary['worker_elf'] = worker_elf

        binaries.append(binary)

    ctx.set_group('bundle')
    ctx.pbl_bundle(
        binaries=binaries,
        js=ctx.path.ant_glob('src/js/**/*.js')
    )
