[![Build Status](https://travis-ci.org/imzhenyu/rDSN.tools.hpc.svg?branch=master)](https://travis-ci.org/imzhenyu/rDSN.tools.hpc)

rDSN.tools.hpc is a collection of high performance local library providers for rDSN, including:

* hpc_network_provider: asynchronous network provider using epoll (Linux) and completion port (Windows)
* hpc_aio_provider: asynchronous disk provider using native aio (Linux) and completion port (Windows)
* hpc_timer_service: TODO
* hpc_logger and hpc_tail_logger: in-memory batched logging facility
* hpc_concurrent_task_queue: based on https://github.com/cameron314/concurrentqueue.git
* other nits

##### Usage

* make sure rDSN is [installed](https://github.com/Microsoft/rDSN/wiki/Installation)
* build dsn.tools.hpc.so (Linux) or dsn.tools.hpc.dll (Windows)
```
    dsn.run.sh build --type release
    dsn.run.sh install
```

  The last command copies the dynamic linked libraries into ```DSN_ROOT/lib```.

* add the modules into correspondent config files for rDSN processes, which registers the providers into rDSN

```
    [modules]
    dsn.tools.hpc
```

* use the providers in config file, e.g.,

```
    [core]
    logging_factory_name = dsn:tools::hpc_logger

    [threadpool..default]
    queue_factory_name = dsn::tools::hpc_concurrent_task_queue
```

   Or, you can directly use all hpc providers by

```
    [core]
    tool = fastrun
```

##### License and Support

rDSN is provided on Windows and Linux, with the MIT open source license. You can use the "issues" tab in GitHub to report bugs.


