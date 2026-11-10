"""Numeric source guard: nonperiodic cloud support, filtered weather and sky history."""
import math
import re
import struct
from pathlib import Path

NAME = 'cloud-field'
STAGE = 'source'
DESCRIPTION = 'cloud coverage does not repeat at the EEP tile; sky history and physical fade remain valid'
GUARDS = '#1283 repetition, far-field weather alias streaks, #1105 sky false rejection and physical horizon cutoff'


def run(ctx, result):
    shader = Path(ctx.path('indra/vkclean/shaders/clean_vol_cloud_common.glsl')).read_text()
    temporal = Path(ctx.path('indra/vkclean/shaders/clean_vol_cloud_temporal.comp')).read_text()
    weather = Path(ctx.path('indra/vkclean/shaders/clean_vol_cloud_weather.comp')).read_text()
    host = Path(ctx.path('indra/newview/vkgfx/llcleanvolcloudsvk.cpp')).read_text()
    try:
        # Evaluate the production scalar shader expressions, not copied warp/coverage constants.
        def expression(name):
            body = re.search(r'float '+name+r'\([^)]*\)\s*\{([^}]*)\}', shader).group(1)
            return re.search(r'return\s+(.*?);', body, re.S).group(1)
        scope = {'clamp': lambda x,a,b: max(a,min(b,x))}
        assert 'vec2(vcWorldPhase(warpCoverage), vcWorldPhase(warpCoverage)' in shader
        assert 'weather_eep.xy + warp' in shader
        warp_y=float(re.search(r'vcWorldPhase\(warpCoverage\) \* ([0-9.]+)',shader).group(1))
        assert 'textureLod(uVcWeather, worldUV, worldLod).a' in shader
        assert 'warpCoverage = wxValue(worldNoise)' in shader
        assert 'clean_vol_cloud_field.glsl' in shader and 'clean_vol_cloud_field.glsl' in weather
        assert shader.count('warp/vc.shape0.z') == 2 # base and erosion agree
        # Actual weather producer hash constants and shifts; CPU uint arithmetic wraps like GLSL.
        noise = Path(ctx.path('indra/vkclean/shaders/clean_vol_cloud_field.glsl')).read_text()
        body = noise[noise.index('uint wxHash('):noise.index('float wxValue(')]
        constants = [int(x) for x in re.findall(r'(\d+)u', body)]
        assert len(constants) == 7
        a,b,s0,c,s1,d,s2 = constants
        def hash2(x,y):
            h=(x*a+y*b)&0xffffffff
            h=((h^(h>>s0))*c)&0xffffffff
            h=((h^(h>>s1))*d)&0xffffffff
            return h^(h>>s2)
        def value(x,y):
            ix,iy=math.floor(x),math.floor(y)
            u,v=x-ix,y-iy;u=u*u*(3-2*u);v=v*v*(3-2*v)
            lo=hash2(ix,iy)*(1-u)+hash2(ix+1,iy)*u
            hi=hash2(ix,iy+1)*(1-u)+hash2(ix+1,iy+1)*u
            return (lo*(1-v)+hi*v)/4294967295
        def detail(u,v):
            return max(0,min(1,.5+.24*math.sin(math.tau*u)+.2*math.cos(math.tau*v)+.15*math.sin(math.tau*(3*u+2*v))))
        producer=re.search(r'float wxCoverage\([^)]*\)\s*\{(.*?)\n\}',weather,re.S).group(1)
        alpha_expr=re.search(r'return\s+(.*?);',producer,re.S).group(1)
        alpha_expr=alpha_expr.replace('wxNoise(rotated,lod).x','eep')
        alpha_code=compile(alpha_expr,'wxCoverage return','eval')
        assert 'dens, wxCoverage(u,vec2(sz))' in weather
        local_cutoff=float(re.search(r'if \(w\.r <= ([0-9.]+)\) return 0.0;',shader).group(1))
        coverage_cutoff=float(re.search(r'if \(cov <= ([0-9.]+)\) return 0.0;',shader).group(1))
        tile=8097.0
        def world(x,y):
            p,q=x/12000,y/12000
            wx,wy=value(p*.7+17.3,q*.7+41.7)-.5,value(p*.7-63.1,q*.7+9.2)-.5
            p+=wx*.75;q+=wy*.75
            fbm=(value(p,q)+.5*value(p*2.03+7.1,q*2.03+7.1)+.25*value(p*4.13-13.7,q*4.13-13.7))/1.75
            u,v=x/tile,y/tile
            eep=detail(math.sqrt(2)*(.8*u-.6*v),math.sqrt(2)*(.6*u+.8*v))
            return eval(alpha_code,{'__builtins__':{}},dict(scope,fbm=fbm,eep=eep))
        # Wind must not turn an 8-bit alpha quantum into a geometry jump.
        f32=lambda x:struct.unpack('f',struct.pack('f',x))[0]
        wind_values=[value(f32(21.3+i*.1/12000),f32(24.7)) for i in range(256)]
        warp_values=[eval(expression('vcWorldPhase'),{'__builtins__':{}},dict(scope,worldCoverage=w))*tile for w in wind_values]
        assert max(abs(b-a) for a,b in zip(warp_values,warp_values[1:]))<.5
        assert abs(value(1-1e-6,2.37)-value(1+1e-6,2.37))<1e-8
        phase_expr=expression('vcWorldPhase');cov_expr=expression('vcCoverage')
        def field(x,y,warped):
            w=world(x,y)
            # Compile once to keep the source-stage check cheap.
            offset=eval(phase_code,{'__builtins__':{}},dict(scope,worldCoverage=value(x/12000,y/12000))) if warped else 0
            local=detail(x/tile+offset,y/tile+offset*warp_y)
            if local<=local_cutoff:return 0.0
            cov=eval(cov_code,{'__builtins__':{}},dict(scope,localCoverage=local,worldCoverage=w))
            return cov if cov>coverage_cutoff else 0.0
        phase_code=compile(phase_expr,'vcWorldPhase','eval');cov_code=compile(cov_expr,'vcCoverage','eval')
        def corr(a,b):
            am=sum(a)/len(a);bm=sum(b)/len(b)
            aa=sum((x-am)**2 for x in a);bb=sum((y-bm)**2 for y in b)
            return sum((x-am)*(y-bm) for x,y in zip(a,b))/math.sqrt(aa*bb)
        scores=[]
        for dx,dy in [(tile,0),(0,tile)]:
            old0=[];old1=[];new0=[];new1=[]
            for j in range(64):
                for i in range(64):
                    x=-64000+i*2000;y=-64000+j*2000
                    old0.append(field(x,y,False));old1.append(field(x+dx,y+dy,False))
                    new0.append(field(x,y,True));new1.append(field(x+dx,y+dy,True))
            old,new=corr(old0,old1),corr(new0,new1)
            assert old>.95, ('fixture must expose former periodic support',old)
            assert abs(new)<.8, ('old tile autocorrelation returned',new)
            scores.append((old,new))
        assert 'vcWeatherLod(footprint' in shader and 'localLod)' in shader
        # Execute the production LOD expression for a distant coarse march step.
        lod_body=re.search(r'float vcWeatherLod\([^)]*\)\s*\{(.*?)\}',shader,re.S).group(1)
        lod_return=re.search(r'return (.*?);',lod_body).group(1)
        lod_return=lod_return.replace('float(textureQueryLevels(uVcWeather) - 1)','9.0')
        lod=eval(lod_return,{'__builtins__':{}},dict(scope,log2=math.log2,max=max,texels=300.0*512/tile))
        assert lod>4.0
        # A controlled 32m weather stripe under a 300m footprint: its box-filter
        # response, rather than one point sample whose phase changes every frame.
        width=2**lod*tile/512
        response=abs(math.sin(math.pi*width/32)/(math.pi*width/32))
        assert response<.05, ('unfiltered far-weather alias returned',lod,response)

        assert 'wxSourceLod(16.0' in weather and 'WEATHER_MIP' in weather
        assert 'kWeatherMipLevels' in host and 'mWxMipSet[mip - 1]' in host
        # The shader history predicate is small enough to evaluate each return directly.
        body=re.search(r'bool vcSceneHistoryValid\([^)]*\)\s*\{(.*?)\n\}',temporal,re.S).group(1)
        returns=re.findall(r'return (.*?);',body)
        assert len(returns)==3
        final=returns[2]
        branches=re.findall(r'if\s*\((.*?)\)\s*return\s+(\w+);',body,re.S)
        def compatible(scene,previous,end):
            env={'abs':abs,'max':max,'scene':scene,'previousScene':previous,'cloudEnd':end}
            for predicate,answer in branches:
                predicate=predicate.replace('||',' or ').replace('&&',' and ').replace('!',' not ')
                if eval(predicate.strip(),{'__builtins__':{}},env): return answer=='true'
            return eval(final, {'__builtins__':{}}, env)
        half=lambda x:struct.unpack('e',struct.pack('e',x))[0]
        assert compatible(65000,half(65000),60000) # all-sky, FP16 sentinel
        assert compatible(25000,12000,10000) # both beyond cloud exit: no truncation
        assert not compatible(1000,65000,10000) # real foreground disocclusion
        assert not compatible(65000,1000,10000)
        assert not compatible(65000,0,10000) # old/uninitialized metadata
        assert 'vcSceneHistoryValid(sd, hB.w, cloudEnd)' in temporal
        atmos=shader[shader.index('VcResult vcAtmosphere('):]
        # The physical branch must reach the range fade as well as the classic branch.
        physical=atmos[atmos.index('if(cmOn())'):atmos.index('else if (atmosEnabled())')]
        assert 'return r;' not in physical
        assert 'smoothstep(maxd * 0.45, maxd, r.dmean)' in atmos
        result.summary='tile autocorrelation X/Y '+str([(round(a,3),round(b,3)) for a,b in scores])+f'; far-weather LOD {lod:.2f}, stripe gain {response:.3f}; sky/occluder/fade checks'
    except (AssertionError, AttributeError, IndexError) as error:
        result.fail(str(error) or 'cloud field source contract missing')
