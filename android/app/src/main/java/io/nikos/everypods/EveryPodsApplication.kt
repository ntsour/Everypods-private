package io.nikos.everypods

import android.app.Application
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import io.nikos.everypods.billing.BillingManager
import io.nikos.everypods.billing.BillingProviderFactory

class EveryPodsApplication: Application(), DefaultLifecycleObserver {
    override fun onCreate() {
        BillingManager.provider = BillingProviderFactory.create(this)
        ProcessLifecycleOwner.get().lifecycle.addObserver(this)

        super<Application>.onCreate()

    }

    override fun onResume(owner: LifecycleOwner) {
        BillingManager.provider.queryPurchases()
    }
}
