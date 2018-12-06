# OverlappedServer
Server using overlapped IOconnect with multiple clients 基于多线程重叠IO模型的服务器  


仍存在bug：发送文件时，正在工作的IO对象数量计数可能为负，该计数用于释放socket对象

待完善：接收文件和包的重排序


服务器逻辑
操作对象：  
THREAD_OBJ：线程对象，用全局线程对象列表维护  
SOCKET_OBJ：socket对象，用全局socket对象列表维护  
BUFFER_OBJ：IO对象，与socket对象绑定，用一个线程对象中的列表维护，与线程对象中的一个event关联  

PostAccept：投递监听IO  
PostRecv：投递接收IO  
PostSend：投递发送IO  

main：调用PostAccept投递监听请求  
ServerThread：等待IO对象受信，并调用HandleIO处理之  
HandleIO：处理已完成的IO  
①监听IO：创建新的socket对象，并为其投递回显IO和监听IO  
②发送IO：释放IO对象  
③接收IO：若为普通消息，投递回显IO；若为/recv:开头的字符串，调用FileSend发送文件，发送失败回送错误信息；原IO对象继续接收  

FileSend：查找是否存在请求文件，不存在则返回；存在则多次调用PostSend投递发送文件包的IO请求  



Overlapped与其他模型的不同之处：  
其他模型以WSAeventselect模型为代表  
两者大体结构相同，都有一个全局的线程列表，每个线程处理处理一个事件列表  
不同：  
①WSAeventselect处理的事件列表为连接列表（socket对象列表），这意味着对于同一个socket上的IO事件只能由同一个线程来处理；  
Overlapped处理的事件列表为IO对象列表，同一个socket上的不同IO事件可以同时由不同线程处理（提高单兵作战能力）  
②WSAeventselect是信号量驱动IO模型，通过socket的授信状态来判断是否有IO事件发生，若有接收事件发生，需要由用户线程把内核缓冲区的数据拷贝到用户缓冲区；  
Overlapped是完全异步IO模型，操作系统接收到数据后直接拷贝到用户态缓冲区，不需要用户线程来进行拷贝的操作，提高用户线程的运行效率，但同时会产生内存锁定问题，需要调整缓冲区大小为页大小倍数  
