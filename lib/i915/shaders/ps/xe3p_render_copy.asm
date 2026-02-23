L0:
(W)     mad (16|M0)              acc0.0<1>:f   r6.0<0;0>:f       r1.0<1;0>:f       r6.6<0>:f
(W)     mad (16|M0)              r113.0<1>:f   acc0.0<1;0>:f     r1.0<1;0>:f       r6.1<0>:f
(W)     mad (16|M0)              acc0.0<1>:f   r6.3<0;0>:f       r1.0<1;0>:f       r6.4<0>:f
(W)     mad (16|M0)              r114.0<1>:f   acc0.0<1;0>:f     r2.0<1;0>:f       r6.5<0>:f
(W)     send.smpl (16|M0)        r12      r113  null:0  0x0            0x04420001           {F@1,$0} // wr:2+0, rd:4; simd16 sample:u,v,r,ai,mlod using sampler index 0
(W)     send.rc (16|M0)          null     r12  null:0  0x0            0x08031400           {EOT,$0} // wr:4+0, rd:0; render write  last render target select to bti[0].rti[0]
L96:
