## 基本约束

1. sample 时，如果用 maskcount 采样，则无法采集到栈尾的 c 调用，所以不可能使用 maskcount 来做 sample 的。只能尝试用线程定时器加上 async-signal-safe function 的方式来做，并且需要直接修改 lua vm。  

2. linux 定时器做 sample时：不要在信号处理器里调用任何 Lua C API（不属于 async-signal-safe，会有未定义行为/死锁/崩溃风险），即便 Lua VM 只在该线程运行也不代表安全。  

3. 要支持 skynet 这种 actor 模型的服务器，profile 时是指定某个服务的，需要能够只采样特定的服务。  

4. CPU sample 的基本思路：
  1、linux 设置定时器；
  2、信号处理函数抓一次 c 栈并记录起来，并且往 lua vm 的 profile_trap 标志位加1；
  3、lua vm 在各个逻辑检测到标志位后，抓一次栈并记录起来；
  4、之所以要在信号处理函数里面抓一次 c 栈，是因为有可能一个 c 函数执行很久，那么光靠 lua vm 检测标志位再抓，是可能会漏抓的；
  5、lua vm 在 call或return等少数地方检测 profile_trap，如果 >= 1，就 dump 一下栈，并把这个栈计数设为 profile_trap 的值，最后把 profile_trap 清 0。相比于 profile_trap 作为标志位，在每条指令检测它，这种累积的方式更节省运算量，
  6、最终的产物就包含了 c 栈记录，lua 栈记录，两份记录各自产出，可以形成对照。  