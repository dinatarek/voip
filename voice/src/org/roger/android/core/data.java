package org.roger.android.core;

import org.roger.android.droid.droid;
import org.roger.android.droid.main;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Bundle;

public class data {
	Context ctx;
	Bundle values = new Bundle();
	public data( main m ) {
		ctx = m;
		values.putString("carrior", "192.168.95.2");
		values.putString("passwd", "roger");
		values.putString("user", "123");
		values.putString("online", "false");
		values.putString("mute", "false");
	}
	public String get( String key ) {
		SharedPreferences perference = ctx.getSharedPreferences("roger", 0);  
		String data = perference.getString(key, null);  
		if( data == null || data.length() == 0 ) {
			SharedPreferences.Editor editor = ctx.getSharedPreferences("roger", 0).edit();  
			editor.putString(key,values.getString(key));  
			editor.commit();
		}else {
			values.putString(key, data);
		}
		return  values.getString(key);
	}
	public void set( String key, String value ) {
		SharedPreferences perference = ctx.getSharedPreferences("roger", 0);  
		String data = perference.getString(key, null);  
		if( data != null || data.length() != 0 ) {
			SharedPreferences.Editor editor = ctx.getSharedPreferences("roger", 0).edit(); 
			editor.remove(key);
			editor.putString(key,value );  
			editor.commit();
		}else {
			SharedPreferences.Editor editor = ctx.getSharedPreferences("roger", 0).edit(); 
			editor.putString(key,value );  
			editor.commit();
		}
		values.remove(key);
		values.putString(key, value );
	}
	public String getCarrior() {
		return get( "carrior" );
	}
	public String getPasswd() {
		return get( "passwd" );
	}
	public String getUser() {
		return get( "user" );
	}
	public boolean getOnline() {
		String r =  get( "online" );
		if ( r.equals("true") )
			return true;
		else
			return false;
	}
	public boolean getMute() {
		String r =  get( "mute" );
		if ( r.equals("true") )
			return true;
		else
			return false;
	}
	public void setCarrior( String val ) {
		set( "carrior", val );
	}
	public void setPasswd( String val ) {
		set( "passwd", val );
	}
	public void setUser( String val ) {
		set( "user", val );
	}
	public void sgetOnline( boolean val ) {
		if ( val )
			set( "online", "true" );
		else
			set( "online", "false" );
	}
	public void setMute( boolean val ) {
		if ( val )
			set( "mute", "true" );
		else
			set( "mute", "false" );
	}
}