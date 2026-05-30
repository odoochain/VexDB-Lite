# 产品能力矩阵

<table>
<thead>
<tr><th>分类</th><th>功能</th><th>描述</th><th>VexDB_pg</th><th>VexDB商用版</th></tr>
</thead>
<tbody>
<tr>
<td>向量检索图索引</td>
<td>graph_index图索引</td>
<td>完全自研高性能图索引，融合多种图结构优势，全场景通用</td>
<td>✅</td>
<td>✅</td>
</tr>
<tr>
<td>距离计算</td>
<td>距离计算函数模板分发</td>
<td>内联距离计算函数，编译时优化</td>
<td>✅</td>
<td>✅</td>
</tr>
<tr>
<td rowspan=3>缓存相关</td>
<td>vector buffer</td>
<td>通用向量缓存，全场景通用</td>
<td>✅</td>
<td>✅</td>
</tr>
<tr>
<td>bulk buffer</td>
<td>全内存向量缓存，内存充足场景下最大加速</td>
<td>❌</td>
<td>✅</td>
</tr>
<tr>
<td>缓存异步IO</td>
<td>内存受限场景下加速磁盘到缓存数据读取</td>
<td>❌</td>
<td>✅</td>
</tr>
<tr>
<td rowspan=3>数据类型</td>
<td>floatvector数据类型</td>
<td>标准数据类型</td>
<td>✅</td>
<td>✅</td>
</tr>
<tr>
<td>halfvector数据类型</td>
<td>半精度数据类型</td>
<td>🟡</td>
<td>✅</td>
</tr>
<tr>
<td>int8vector数据类型</td>
<td>int8数据类型</td>
<td>🟡</td>
<td>✅</td>
</tr>
<tr>
<td rowspan=3>量化</td>
<td>pq量化</td>
<td>向量压缩比最大，QPS与原始向量相近</td>
<td>✅</td>
<td>✅</td>
</tr>
<tr>
<td>rabitq量化</td>
<td>向量压缩比中等，QPS优于原始向量</td>
<td>🟡</td>
<td>✅</td>
</tr>
<tr>
<td>量化自动开启功能</td>
<td>后台自动开启量化，支持空表建量化索引</td>
<td>❌</td>
<td>✅</td>
</tr>
<tr>
<td rowspan=4>图索引增强功能</td>
<td>图索引异步插入功能</td>
<td>多写少读场景下快速入库</td>
<td>❌</td>
<td>✅</td>
</tr>
<tr>
<td>图挂桶功能</td>
<td>小规格机器承载大规模向量检索</td>
<td>❌</td>
<td>✅</td>
</tr>
<tr>
<td>主备高可用</td>
<td>支持主备同步与备份恢复</td>
<td>❌</td>
<td>✅</td>
</tr>
<tr>
<td>并行vacuum</td>
<td>并行加速索引清理回收</td>
<td>❌</td>
<td>✅</td>
</tr>
</tbody>
</table>

- ✅ = 已支持 🟡 = 即将支持 ❌ = 不支持
