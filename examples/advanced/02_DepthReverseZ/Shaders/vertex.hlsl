cbuffer Settings:register(b15){float4 value;}
struct Output{float4 position:SV_Position;float3 color:COLOR0;};
Output main(uint id:SV_VertexID,uint instance:SV_InstanceID){float2 corners[6]={float2(-1,-1),float2(1,-1),float2(-1,1),float2(-1,1),float2(1,-1),float2(1,1)};
bool nearFace=(instance&1)==1,distant=instance>=2;float n=value.z,f=value.w;
float z=distant?10000.0+(nearFace?0.0:.002):(nearFace?2.0:3.0);float2 ndc=float2(distant?.48:-.48,nearFace?.06:-.06)+corners[id]*float2(.34,.5);
float depth=value.x>.5?(f*n-n*z)/(f-n):(f*z-f*n)/(f-n);Output o;o.position=float4(ndc*z,depth,z);o.color=nearFace?float3(.1,.85,.35):float3(.9,.12,.2);return o;}
