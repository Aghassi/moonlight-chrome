function guuid() {
	return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function(c) {
	    var r = Math.random()*16|0, v = c === 'x' ? r : (r&0x3|0x8);
	    return v.toString(16);
	});
}

function uniqueid() {
    return 'xxxxxxxxxxxxxxxx'.replace(/[x]/g, function(c) {
	    var r = Math.random()*16|0;
	    return r.toString(16);
	});
}

String.prototype.toHex = function() {
	var hex = '';
	for(var i = 0; i < this.length; i++) {
		hex += '' + this.charCodeAt(i).toString(16);
	}
	return hex;
}

function NvHTTP(address, clientUid) {
    this.address = address;
    this.paired = false;
    this.supports4K = false;
    this.currentGame = 0;
    this.serverMajorVersion = 0;
    this.clientUid = clientUid;
    this._baseUrlHttps = 'https://' + address + ':47984';
    this._baseUrlHttp = 'http://' + address + ':47989';
    this._appListCache = null;
    _self = this;
};

NvHTTP.prototype = {
    refreshServerInfo: function () {
        return sendMessage('openUrl', [ _self._baseUrlHttps + '/serverinfo?' + _self._buildUidStr()]).then(function(ret) {
            if (!_self._parseServerInfo(ret)) {
                return sendMessage('openUrl', [ _self._baseUrlHttp + '/serverinfo?' + _self._buildUidStr()]).then(function(retHttp) {
                    _self._parseServerInfo(retHttp);
                });
            }
        });
    },
    
    _parseServerInfo: function(xmlStr) {
        $xml = _self._parseXML(xmlStr);
        $root = $xml.find('root');
        
        if($root.attr("status_code") != 200) {
            return false;
        }
        
        _self.paired = $root.find("PairStatus").text().trim() === 1;
        _self.currentGame = parseInt($root.find("currentgame").text().trim(), 10);
        _self.serverMajorVersion = parseInt($root.find("appversion").text().trim().substring(0, 1), 10);
        
        // GFE 2.8 started keeping currentgame set to the last game played. As a result, it no longer
        // has the semantics that its name would indicate. To contain the effects of this change as much
        // as possible, we'll force the current game to zero if the server isn't in a streaming session.
        if ($root.find("state").text().trim().endsWith("_SERVER_AVAILABLE")) {
            _self.currentGame = 0;
        }
        
        return true;
    },
    
    getAppById: function (appId) {
        return _self.getAppList().then(function (list) {
            var retApp = null;
            
            list.some(function (app) {
                if (app.id === appId) {
                    retApp = app;
                    return true;
                }
                
                return false;
            });
            
            return retApp;
        });
    },
    
    getAppByName: function (appName) {
        return _self.getAppList().then(function (list) {
            var retApp = null;
            
            list.some(function (app) {
                if (app.title === appName) {
                    retApp = app;
                    return true;
                }
                
                return false;
            });
            
            return retApp;
        });
    },
    
    getAppList: function () {
        if (_self._appListCache) {
            console.log('Returning app list from cache');
            return new Promise(function (resolve, reject) {
                resolve(_self._appListCache);
            });
        }
        
        return sendMessage('openUrl', [_self._baseUrlHttps + '/applist?' + _self._buildUidStr()]).then(function (ret) {
            $xml = _self._parseXML(ret);
            
            var rootElement = $xml.find("root")[0];
            var appElements = rootElement.getElementsByTagName("App");
            var appList = [];
            
            for (var i = 0, len = appElements.length; i < len; i++) {
                appList.push({
                    title: appElements[i].getElementsByTagName("AppTitle")[0].innerHTML.trim(),
                    id: parseInt(appElements[i].getElementsByTagName("ID")[0].innerHTML.trim(), 10),
                    running: (appElements[i].getElementsByTagName("IsRunning")[0].innerHTML.trim() === 1)
                });
            }
            
            if (appList)
                _self._appListCache = appList;
            
            return appList;
        });
    },
    
    getBoxArt: function (appId) {
        return sendMessage('openUrl', [
            _self._baseUrlHttps +
            '/appasset?'+_self._buildUidStr() +
            '&appid=' + appId + 
            '&AssetType=2&AssetIdx=0'
        ]).then(function (ret) {
            return ret;
        });
    },
    
    launchApp: function (appId, mode, sops, rikey, rikeyid, localAudio, surroundAudioInfo) {
        return sendMessage('openUrl', [
            _self._baseUrlHttps +
            '/launch?' + _self._buildUidStr() +
            '&appid=' + appId +
            '&mode=' + mode +
            '&additionalStates=1&sops=' + sops +
            '&rikey=' + rikey +
            '&rikeyid=' + rikeyid +
            '&localAudioPlayMode=' + localAudio +
            '&surroundAudioInfo=' + surroundAudioInfo
        ]).then(function (ret) {
            return true;
        });
    },
    
    resumeApp: function (rikey, rikeyid) {
        return sendMessage('openUrl', [
            _self._baseUrlHttps +
            '/resume?' + _self._buildUidStr() +
            '&rikey=' + rikey +
            '&rikeyid=' + rikeyid
        ]).then(function (ret) {
            return true;
        });
    },
    
    quitApp: function () {
        return sendMessage('openUrl', [_self._baseUrlHttps + '/cancel?' + _self._buildUidStr()]).then(function () {
            _self.currentGame = 0;
        });
    },
    
    pair: function(randomNumber) {
        return _self.refreshServerInfo().then(function () {
            if (_self.paired)
                return true;
            
            if (_self.currentGame != 0)
                return false;
            
            return sendMessage('pair', [_self.serverMajorVersion, _self.address, randomNumber]).then(function (pairStatus) {
                return sendMessage('openUrl', [_self._baseUrlHttps + '/pair?uniqueid=' + _self.clientUid + '&devicename=roth&updateState=1&phrase=pairchallenge']).then(function (ret) {
                    $xml = _self._parseXML(ret);
                    _self.paired = $xml.find('paired').html() === "1";
                    return _self.paired;
                });
            });
        });
    },
    
    unpair: function () {
        return sendMessage('openUrl', [_self._baseUrlHttps + '/unpair?' + _self._buildUidStr()]);
    },
    
    _buildUidStr: function () {
        return 'uniqueid=' + _self.clientUid + '&uuid=' + guuid();
    },
    
    _parseXML: function (xmlData) {
        return $($.parseXML(xmlData.toString()));
    },
};
