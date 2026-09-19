cbuffer Settings:register(b15){float4 value;}
float main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Depth{
float n=value.z,f=value.w;float distance=lerp(n+1,f*.9,uv.x);if(length(uv-float2(.3,.5))<.2)distance=n+2;
return (f-f*n/distance)/(f-n);}
