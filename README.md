# 说明

这是 [intel/modsecurity-wasm-filter](https://github.com/intel/modsecurity-wasm-filter) 的 fork。

创建镜像命令：

```shell
cd wasmplugin
docker build --build-arg http_proxy=http://10.252.1.248:8889 --build-arg http_proxys=http://10.252.1.248:8889 -t ndssl-wasm:waf-filter -f Dockerfile .
```